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

/* CRUD operates on the attached provider, including its real storage and
 * exceptions. An empty cursor or a success count from the framework would
 * discard the application's reads/writes. */
static bool x_resolver_operation(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                  int nargs,union dvm_value *out)
{
   (void)self;
   const struct dvm_method *operation=dvm__builtin_method();
   if (!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","uri");return false;}
   union dvm_value scheme={0},authority={0};
   if (!am_vcall(vm,ARG(0).l,"getScheme","()Ljava/lang/String;",NULL,0,&scheme) ||
       !am_vcall(vm,ARG(0).l,"getAuthority","()Ljava/lang/String;",NULL,0,&authority)) return false;
   const char *protocol=dvm_string_utf8(vm,scheme.l);
   const char *name=dvm_string_utf8(vm,authority.l);
   dvm_ref provider=protocol && !strcmp(protocol,"content") && name ? x_find_provider(vm,name) : 0;
   if (!provider) {
      if (!strcmp(operation->name,"query")) RETL(0);
      dvm__throw(vm,"java/lang/IllegalArgumentException","Unknown content provider %s",name ? name : "");
      return false;
   }
   return am_vcall(vm,provider,operation->name,operation->sig,args,nargs,out);
}

static const struct rt_method rt_x_content_resolver[] = {
   M("call", "(Landroid/net/Uri;Ljava/lang/String;Ljava/lang/String;Landroid/os/Bundle;)Landroid/os/Bundle;", x_resolver_call),
   M("insert","(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;",x_resolver_operation),
   M("update","(Landroid/net/Uri;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;)I",x_resolver_operation),
   M("delete","(Landroid/net/Uri;Ljava/lang/String;[Ljava/lang/String;)I",x_resolver_operation),
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


/* ------------------------------------------------------------ WindowManager */

/* The windows added straight to a WindowManager (an SDK's login overlay, a
 * floating button).  Each view gets a window object of its own that carries the
 * view and its WindowManager.LayoutParams, which is all the UI layer asks of a
 * window; presenting it, measuring it against the display and hiding it again
 * are the UI layer's, exactly as for a Dialog or a PopupWindow. */
static const struct rt_field rt_managed_window_fields[] = {
   { "contentView", "Landroid/view/View;" },
   { "attributes", "Landroid/view/WindowManager$LayoutParams;" },
   F_END
};

static struct { dvm_ref view, window; } g_wm_added[UI_MAX_WINDOWS];
static int g_wm_nadded;

static int wm_find(dvm_ref view)
{
   for (int i = 0; i < g_wm_nadded; ++i)
      if (g_wm_added[i].view == view) return i;
   return -1;
}

/* A WindowManager.LayoutParams for `lp`: itself when it already is one, else a
 * fresh one with the same size (a plain ViewGroup.LayoutParams has no window
 * attributes of its own). */
static dvm_ref wm_attributes(struct dvm *vm, dvm_ref lp)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/view/WindowManager$LayoutParams;");
   if (!c || ui_is_a(vm, lp, "android/view/WindowManager$LayoutParams")) return lp;
   dvm_ref fresh = dvm_new_object(vm, c);
   struct dvm_method *init = fresh ? dvm_find_method(vm, c, "<init>", "()V") : NULL;
   union dvm_value ignored = {0};
   if (!fresh || !init || !dvm_call(vm, init, fresh, NULL, 0, &ignored)) return 0;
   rt_set_int(vm, fresh, "width", rt_get_int(vm, lp, "width"));
   rt_set_int(vm, fresh, "height", rt_get_int(vm, lp, "height"));
   return fresh;
}

static bool x_wm_addView(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/IllegalArgumentException", "View must not be null"); return false; }
   if (!ARG(1).l) { dvm__throw(vm, "java/lang/IllegalArgumentException", "Params must not be null"); return false; }
   if (wm_find(ARG(0).l) >= 0 || ui_obj(vm, ARG(0).l, "parent", "Landroid/view/ViewParent;")) {
      dvm__throw(vm, "java/lang/IllegalStateException",
                 "The specified view has already been added to a parent or the WindowManager");
      return false;
   }
   if (g_wm_nadded >= UI_MAX_WINDOWS) {
      dvm__throw(vm, "android/view/WindowManager$BadTokenException", "too many windows");
      return false;
   }
   struct dvm_class *c = dvm__class_by_desc(vm, "Llunaria/ManagedWindow;");
   dvm_ref window = c ? dvm_new_object(vm, c) : 0;
   dvm_ref attributes = window ? wm_attributes(vm, ARG(1).l) : 0;
   if (!window || !attributes) return false;
   (void)dvm_set_field(vm, window, "contentView", "Landroid/view/View;", ARG(0));
   (void)dvm_set_field(vm, window, "attributes", "Landroid/view/WindowManager$LayoutParams;",
                       (union dvm_value){ .l = attributes });
   dvm_pin(vm, ARG(0).l);
   dvm_pin(vm, window);
   g_wm_added[g_wm_nadded].view = ARG(0).l;
   g_wm_added[g_wm_nadded++].window = window;
   ui_window_shown(vm, window);
   RETV();
}

static bool x_wm_removeView(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   const int i = ARG(0).l ? wm_find(ARG(0).l) : -1;
   if (i < 0) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "View not attached to window manager");
      return false;
   }
   const dvm_ref view = g_wm_added[i].view, window = g_wm_added[i].window;
   ui_window_hidden(vm, window);
   memmove(&g_wm_added[i], &g_wm_added[i + 1], (size_t)(g_wm_nadded - i - 1) * sizeof g_wm_added[0]);
   --g_wm_nadded;
   dvm_unpin(vm, window);
   dvm_unpin(vm, view);
   RETV();
}

static bool x_wm_updateViewLayout(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   const int i = ARG(0).l ? wm_find(ARG(0).l) : -1;
   if (i < 0 || !ARG(1).l) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", i < 0 ? "View not attached to window manager"
                                                                : "Params must not be null");
      return false;
   }
   dvm_ref attributes = wm_attributes(vm, ARG(1).l);
   if (!attributes) return false;
   (void)dvm_set_field(vm, g_wm_added[i].window, "attributes", "Landroid/view/WindowManager$LayoutParams;",
                       (union dvm_value){ .l = attributes });
   ui_layout_changed();
   ui_invalidate();
   RETV();
}

static const struct rt_method rt_x_window_manager[] = {
   M("addView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V", x_wm_addView),
   M("removeView", "(Landroid/view/View;)V", x_wm_removeView),
   M("removeViewImmediate", "(Landroid/view/View;)V", x_wm_removeView),
   M("updateViewLayout", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V", x_wm_updateViewLayout),
   M_END,
};

/* --------------------------------------------------------------- ImageFormat */

/* Bits per pixel of the formats Camera, MediaCodec and ImageReader name; -1
 * for the ones with no fixed density (JPEG, the private and compressed ones). */
static bool x_imageformat_bits(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   switch (args[0].i) {
   case 4:  case 16: case 20: case 0x44363159: case 0x20363159: case 0x101: RETI(16);  /* RGB_565 NV16 YUY2 DEPTH16 Y16 DEPTH_POINT_CLOUD */
   case 17: case 0x32315659: case 35: RETI(12);                                       /* NV21 YV12 YUV_420_888 */
   case 39: RETI(16);                                                                 /* YUV_422_888 */
   case 40: case 41: RETI(24);                                                        /* YUV_444_888 FLEX_RGB_888 */
   case 42: RETI(32);                                                                 /* FLEX_RGBA_8888 */
   case 32: RETI(16);                                                                 /* RAW_SENSOR */
   case 37: RETI(10);                                                                 /* RAW10 */
   case 38: RETI(12);                                                                 /* RAW12 */
   case 0x20203859: RETI(8);                                                          /* Y8 */
   default: RETI(-1);
   }
}

static const struct rt_method rt_x_image_format[] = {
   SM("getBitsPerPixel", "(I)I", x_imageformat_bits),
   M_END,
};

/* ------------------------------------------------------ AudioFormat / AudioTrack */

/* android.media.AudioFormat and the API 21+ AudioTrack surface.  Media3's
 * DefaultAudioSink builds its track as
 *   new AudioTrack(AudioAttributes, new AudioFormat.Builder()...build(), size, mode, session)
 * (or AudioTrack.Builder) and feeds it ByteBuffers; the legacy
 * AudioTrack(int,int,int,int,int,int) constructor and write(byte[]) were the only
 * entry points modelled, so the first video with sound stopped in
 * AudioFormat$Builder.setChannelMask.  Everything here lands on the same
 * luna_audio_track the legacy path drives. */

static const struct rt_field rt_audio_format_fields[] = {
   { "rate", "I" }, { "mask", "I" }, { "encoding", "I" }, F_END
};

static bool af_builder_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   rt_set_int(vm, self, "encoding", 1);          /* ENCODING_DEFAULT */
   RETV();
}

static bool af_builder_set(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *n = builtin_name();
   if (!strcmp(n, "setEncoding")) rt_set_int(vm, self, "encoding", args[0].i);
   else if (!strcmp(n, "setSampleRate")) {
      if (args[0].i <= 0) { dvm__throw(vm, "java/lang/IllegalArgumentException", "Invalid sample rate %d", args[0].i); return false; }
      rt_set_int(vm, self, "rate", args[0].i);
   } else if (!strcmp(n, "setChannelMask")) rt_set_int(vm, self, "mask", args[0].i);
   RETL(self);                                   /* setChannelIndexMask and friends are ignored */
}

static bool af_builder_build(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/media/AudioFormat;");
   dvm_ref f = c ? dvm_new_object(vm, c) : 0;
   if (!f) return false;
   static const char *const names[] = { "rate", "mask", "encoding" };
   for (int i = 0; i < 3; ++i) rt_set_int(vm, f, names[i], rt_get_int(vm, self, names[i]));
   RETL(f);
}

static bool af_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *n = builtin_name();
   if (!strcmp(n, "getSampleRate")) RETI(rt_get_int(vm, self, "rate"));
   if (!strcmp(n, "getEncoding")) RETI(rt_get_int(vm, self, "encoding"));
   if (!strcmp(n, "getChannelCount")) RETI(audiotrack_channel_count(rt_get_int(vm, self, "mask")));
   RETI(rt_get_int(vm, self, "mask"));           /* getChannelMask */
}

static const struct rt_method rt_audio_format_builder[] = {
   M("<init>", "()V", af_builder_init),
   M("setEncoding", "(I)Landroid/media/AudioFormat$Builder;", af_builder_set),
   M("setSampleRate", "(I)Landroid/media/AudioFormat$Builder;", af_builder_set),
   M("setChannelMask", "(I)Landroid/media/AudioFormat$Builder;", af_builder_set),
   M("setChannelIndexMask", "(I)Landroid/media/AudioFormat$Builder;", af_builder_set),
   M("build", "()Landroid/media/AudioFormat;", af_builder_build),
   M_END,
};

static const struct rt_method rt_audio_format[] = {
   M("getSampleRate", "()I", af_get), M("getEncoding", "()I", af_get),
   M("getChannelMask", "()I", af_get), M("getChannelCount", "()I", af_get),
   M_END,
};

/* AudioTrack(AudioAttributes, AudioFormat, bufferSizeInBytes, mode, sessionId). */
static bool at_init_format(struct dvm *vm, dvm_ref self, int rate, int mask, int encoding,
                           int buffer_size, int mode)
{
   union dvm_value query[3] = { { .i = rate }, { .i = mask }, { .i = encoding } }, min = {0};
   if (buffer_size <= 0) {
      (void)audiotrack_getMinBufferSize(vm, self, query, 3, &min);
      buffer_size = min.i > 0 ? min.i : 0;
   }
   union dvm_value init[6] = { { .i = 3 }, { .i = rate }, { .i = mask }, { .i = encoding },
                               { .i = buffer_size }, { .i = mode } }, ignored = {0};
   return audiotrack_init(vm, self, init, 6, &ignored);
}

static bool at_init_modern(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)out;
   dvm_ref fmt = args[1].l;
   if (!fmt) { dvm__throw(vm, "java/lang/IllegalArgumentException", "Illegal null AudioFormat"); return false; }
   if (!at_init_format(vm, self, rt_get_int(vm, fmt, "rate"), rt_get_int(vm, fmt, "mask"),
                       rt_get_int(vm, fmt, "encoding"), args[2].i, nargs > 3 ? args[3].i : 1))
      return false;
   if (nargs > 4 && args[4].i > 0) rt_set_int(vm, self, "mSessionId", args[4].i);
   RETV();
}

static const struct rt_field rt_audio_track_builder_fields[] = {
   { "format", "Landroid/media/AudioFormat;" }, { "size", "I" }, { "mode", "I" }, { "session", "I" }, F_END
};

static bool atb_set(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *n = builtin_name();
   if (!strcmp(n, "setAudioFormat")) (void)dvm_set_field(vm, self, "format", "Landroid/media/AudioFormat;", args[0]);
   else if (!strcmp(n, "setBufferSizeInBytes")) rt_set_int(vm, self, "size", args[0].i);
   else if (!strcmp(n, "setTransferMode")) rt_set_int(vm, self, "mode", args[0].i);
   else if (!strcmp(n, "setSessionId")) rt_set_int(vm, self, "session", args[0].i);
   RETL(self);              /* setAudioAttributes, setPerformanceMode, setOffloadedPlayback ... */
}

static bool atb_build(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value fmt = {0};
   (void)dvm_get_field(vm, self, "format", "Landroid/media/AudioFormat;", &fmt);
   if (!fmt.l) { dvm__throw(vm, "java/lang/UnsupportedOperationException", "Audio format must be set"); return false; }
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/media/AudioTrack;");
   dvm_ref track = c ? dvm_new_object(vm, c) : 0;
   if (!track) return false;
   const int mode = rt_get_int(vm, self, "mode");
   if (!at_init_format(vm, track, rt_get_int(vm, fmt.l, "rate"), rt_get_int(vm, fmt.l, "mask"),
                       rt_get_int(vm, fmt.l, "encoding"), rt_get_int(vm, self, "size"), mode ? mode : 1))
      return false;
   if (rt_get_int(vm, self, "session") > 0) rt_set_int(vm, track, "mSessionId", rt_get_int(vm, self, "session"));
   RETL(track);
}

static const struct rt_method rt_audio_track_builder[] = {
   M("<init>", "()V", nop_void),
   M("setAudioAttributes", "(Landroid/media/AudioAttributes;)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setAudioFormat", "(Landroid/media/AudioFormat;)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setBufferSizeInBytes", "(I)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setTransferMode", "(I)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setSessionId", "(I)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setPerformanceMode", "(I)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setOffloadedPlayback", "(Z)Landroid/media/AudioTrack$Builder;", atb_set),
   M("build", "()Landroid/media/AudioTrack;", atb_build),
   M_END,
};

/* write(ByteBuffer, size, mode[, timestamp]): the buffer's remaining bytes go
 * through the byte[] writer and its position advances by what was taken. */
static bool at_write_buffer(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   dvm_ref buffer = args[0].l;
   if (!buffer) { dvm__throw(vm, "java/lang/IllegalArgumentException", "buffer is null"); return false; }
   const int8_t *bytes;
   int32_t remaining, pos = 0;
   if (!bb_window(vm, buffer, &bytes, &remaining)) RETI(AT_ERROR_BAD_VALUE);
   (void)stream_field(vm, buffer, "pos", &pos);
   int32_t size = args[1].i;
   if (size < 0 || size > remaining) RETI(AT_ERROR_BAD_VALUE);
   if (!size) RETI(0);
   dvm_ref copy = dvm_new_array(vm, 'B', "B", (uint32_t)size);
   if (!copy) return false;
   dvm_pin(vm, copy);
   memcpy(dvm_array_data(vm, copy), bytes, (size_t)size);
   union dvm_value w[4] = { { .l = copy }, { .i = 0 }, { .i = size }, { .i = nargs > 2 ? args[2].i : 0 } };
   union dvm_value written = {0};
   const bool ok = audiotrack_write(vm, self, w, 4, &written);
   dvm_unpin(vm, copy);
   if (!ok) return false;
   if (written.i > 0) stream_set_field(vm, buffer, "pos", pos + written.i);
   RETI(written.i);
}

static bool at_false(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(0); }

static bool at_channel_config(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "mChannelConfig"));
}

static const struct rt_method rt_x_audio_track[] = {
   M("<init>", "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;III)V", at_init_modern),
   M("<init>", "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;II)V", at_init_modern),
   M("write", "(Ljava/nio/ByteBuffer;II)I", at_write_buffer),
   M("write", "(Ljava/nio/ByteBuffer;III)I", at_write_buffer),
   M("write", "(Ljava/nio/ByteBuffer;IIJ)I", at_write_buffer),
   M("getTimestamp", "(Landroid/media/AudioTimestamp;)Z", at_false),
   M("getUnderrunCount", "()I", at_false),
   M("isOffloadedPlayback", "()Z", at_false),
   M("setOffloadDelayPadding", "(II)V", nop_void),
   M("setOffloadEndOfStream", "()V", nop_void),
   M("setPlaybackParams", "(Landroid/media/PlaybackParams;)V", nop_void),
   M("setPreferredDevice", "(Landroid/media/AudioDeviceInfo;)Z", at_false),
   M("attachAuxEffect", "(I)I", at_false),
   M("setAuxEffectSendLevel", "(F)I", at_false),
   M("addOnRoutingChangedListener", "(Landroid/media/AudioRouting$OnRoutingChangedListener;Landroid/os/Handler;)V", nop_void),
   M("removeOnRoutingChangedListener", "(Landroid/media/AudioRouting$OnRoutingChangedListener;)V", nop_void),
   M("getBufferCapacityInFrames", "()I", audiotrack_getBufferSizeInFrames),
   M("setBufferSizeInFrames", "(I)I", audiotrack_getBufferSizeInFrames),
   M("getChannelConfiguration", "()I", at_channel_config),
   M_END,
};

/* ---------------------------------------------------- CloseGuard / platform hooks */

/* Small platform APIs apps and SDKs call that had no implementation: the
 * dalvik.system.CloseGuard leak detector, Configuration.setToDefaults(),
 * FileObserver(String), and the Activity entry points with nothing to do on
 * this device. */

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

#endif

/* MPL-2.0. Alarm service for the running virtual device. All state is under
 * the VM lock. The Looper polls clock domains without a timer thread per app. */
struct dvm_alarm {
   struct dvm_alarm *next;
   dvm_ref token,listener,handler;
   int type;
   int64_t when,interval,min_elapsed;
};
static void alarm_free(struct dvm *vm,struct dvm_alarm *a)
{
   if(a->token)dvm_unpin(vm,a->token);
   if(a->listener)dvm_unpin(vm,a->listener);
   if(a->handler)dvm_unpin(vm,a->handler);
   free(a);
}
static void alarm_remove(struct dvm *vm,dvm_ref token,dvm_ref listener)
{
   struct dvm_alarm **link=&vm->alarms;
   while(*link) {
      struct dvm_alarm *a=*link;
      if((token && a->token==token) || (listener && a->listener==listener)) {
         *link=a->next;alarm_free(vm,a);
      } else link=&a->next;
   }
}
static void alarm_cancel_token(struct dvm *vm,dvm_ref token)
{ alarm_remove(vm,token,0); }
void dvm_alarm_finish(struct dvm *vm)
{
   while(vm->alarms){struct dvm_alarm *a=vm->alarms;vm->alarms=a->next;alarm_free(vm,a);}
}
static bool pending_send_token(struct dvm *vm,dvm_ref token,int alarm_count)
{
   if(!token || rt_get_int(vm,token,"cancelled")) {
      dvm__throw(vm,"android/app/PendingIntent$CanceledException","PendingIntent is cancelled");return false;
   }
   dvm_ref original=pending_field(vm,token,"intent","Landroid/content/Intent;");
   dvm_ref intent=dvm_new_object(vm,dvm_find_class(vm,"android/content/Intent"));
   if(!intent)return false;dvm_pin(vm,intent);
   if(!intent_copy_into(vm,intent,original,false)){dvm_unpin(vm,intent);return false;}
   uint32_t flags=(uint32_t)rt_get_int(vm,token,"flags");
   /* FLAG_IMMUTABLE ignores sender fill-in extras, including alarm counts. */
   if(alarm_count>0 && !(flags&UINT32_C(0x04000000)))
      intent_put_int(vm,intent,"android.intent.extra.ALARM_COUNT",alarm_count);
   if(flags&UINT32_C(0x40000000))pending_cancel_token(vm,token);
   dvm_ref context=pending_field(vm,token,"context","Landroid/content/Context;");
   union dvm_value a={.l=intent},ignored={0};
   bool handled=dvm_runtime_context_broadcast(vm,"sendBroadcast","(Landroid/content/Intent;)V",context,&a,1,&ignored);
   dvm_unpin(vm,intent);return handled && !dvm_exception(vm);
}
static bool alarm_listener_run(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;(void)out;
   dvm_ref listener=pending_field(vm,self,"listener","Landroid/app/AlarmManager$OnAlarmListener;");
   struct dvm_class *cls=dvm_object_class(vm,listener);
   struct dvm_method *method=cls ? dvm_find_method(vm,cls,"onAlarm","()V") : NULL;
   if(!method){dvm__throw(vm,"java/lang/AbstractMethodError","OnAlarmListener.onAlarm");return false;}
   union dvm_value ignored={0};return dvm_call(vm,method,listener,NULL,0,&ignored);
}
static bool alarm_queue_listener(struct dvm *vm,dvm_ref listener,dvm_ref handler)
{
   dvm_ref job=dvm_new_object(vm,dvm_find_class(vm,"lunaria/AlarmDelivery"));
   if(!job)return false;dvm_pin(vm,job);
   dvm_set_field(vm,job,"listener","Landroid/app/AlarmManager$OnAlarmListener;",(union dvm_value){.l=listener});
   bool ok;
   if(handler) {
      struct dvm_class *cls=dvm_object_class(vm,handler);
      struct dvm_method *post=cls ? dvm_find_method(vm,cls,"post","(Ljava/lang/Runnable;)Z") : NULL;
      union dvm_value arg={.l=job},result={0};
      ok=post && dvm_call(vm,post,handler,&arg,1,&result) && result.i;
   } else ok=dvm__queue_runnable_at(vm,job,false,0);
   dvm_unpin(vm,job);return ok;
}
void dvm_alarm_poll_at(struct dvm *vm,int64_t wall_ms,int64_t elapsed_ms)
{
   if(!vm || dvm_exception(vm) || dvm_is_stopping(vm))return;
   /* Queue delivery, never invoke application callbacks in the scheduler. A
    * fresh search also remains safe when a one-shot token cancels its alarm. */
   for(unsigned dispatched=0;dispatched<500;++dispatched) {
      struct dvm_alarm **link=&vm->alarms;
      while(*link) {
         struct dvm_alarm *a=*link;
         if(a->token && rt_get_int(vm,a->token,"cancelled")){*link=a->next;alarm_free(vm,a);continue;}
         int64_t now=a->type<2 ? wall_ms : elapsed_ms;
         if(now>=a->when && elapsed_ms>=a->min_elapsed)break;
         link=&a->next;
      }
      struct dvm_alarm *a=*link;if(!a)break;
      dvm_ref token=a->token,listener=a->listener,handler=a->handler;
      if(token)dvm_pin(vm,token);if(listener)dvm_pin(vm,listener);if(handler)dvm_pin(vm,handler);
      int64_t now=a->type<2 ? wall_ms : elapsed_ms;
      int64_t count=a->interval>0 ? 1+(now-a->when)/a->interval : 1;
      if(a->interval>0 && now<INT64_MAX) {
         int64_t increment=a->interval-(now-a->when)%a->interval;
         a->when=now>INT64_MAX-increment ? INT64_MAX : now+increment;
         a->min_elapsed=0;
      } else {*link=a->next;alarm_free(vm,a);}
      bool ok=token ? pending_send_token(vm,token,count>INT_MAX ? INT_MAX : (int)count)
                    : alarm_queue_listener(vm,listener,handler);
      if(!ok) {
         struct dvm_class *error=dvm_object_class(vm,dvm_exception(vm));
         fprintf(stderr,"[alarm] unable to enqueue delivery: %s\n",error ? error->name : "queue unavailable");
         dvm_clear_exception(vm);
      }
      if(token)dvm_unpin(vm,token);if(listener)dvm_unpin(vm,listener);if(handler)dvm_unpin(vm,handler);
   }
}
void dvm_alarm_poll(struct dvm *vm)
{
   if(!vm || !vm->alarms)return;
   dvm_alarm_poll_at(vm,(int64_t)(luna_os_realtime_ns()/1000000),
                        (int64_t)(luna_os_boottime_ns()/1000000));
}
static bool alarm_can_exact(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)vm;(void)self;(void)args;(void)nargs;
   RETI(lunaria_app_target_sdk()<31 || arm_exec_guest_uid()<10000 ||
        manifest_has_permission("android.permission.SCHEDULE_EXACT_ALARM"));
}
static bool alarm_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)out;
   struct dvm_method *method=dvm__builtin_method();
   const char *name=method->name,*sig=method->sig;
   bool repeating=!strcmp(name,"setRepeating") || !strcmp(name,"setInexactRepeating");
   bool window=!strcmp(name,"setWindow"),direct=strstr(sig,"OnAlarmListener;")!=NULL;
   int operation_index=(repeating || window) ? 3 : 2;
   dvm_ref operation=direct ? 0 : ARG(operation_index).l;
   dvm_ref listener=direct ? ARG(nargs-2).l : 0,handler=direct ? ARG(nargs-1).l : 0;
   if(!operation && !listener)RETV(); /* AlarmManagerService's invalid-pair contract. */
   bool exact=!strcmp(name,"setExact") || !strcmp(name,"setExactAndAllowWhileIdle") ||
              (window && ARG(2).j==0) || (!strcmp(name,"set") && lunaria_app_target_sdk()<19);
   union dvm_value permitted={0};
   if(exact && (!alarm_can_exact(vm,0,NULL,0,&permitted) || !permitted.i)) {
      dvm__throw(vm,"java/lang/SecurityException","SCHEDULE_EXACT_ALARM permission required");return false;
   }
   int type=ARG(0).i;
   if(type<0 || type>3){dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid alarm type %d",type);return false;}
   dvm_ref token=operation ? pending_field(vm,operation,"token","Landroid/app/PendingIntent;") : 0;
   if(operation && (!token || rt_get_int(vm,token,"cancelled")))RETV();
   int64_t wall=(int64_t)(luna_os_realtime_ns()/1000000),elapsed=(int64_t)(luna_os_boottime_ns()/1000000);
   int64_t futurity=arm_exec_guest_uid()<10000 ? 0 : 5000;
   int64_t minimum=(type<2 ? wall : elapsed)+futurity;
   int64_t when=ARG(1).j;if(when<minimum)when=minimum;
   int64_t interval=repeating ? ARG(2).j : 0;
   if(interval<0)interval=0;
   if(interval>0 && interval<60000)interval=60000;
   if(interval>INT64_C(31536000000))interval=INT64_C(31536000000);
   size_t count=0;bool replacement=false;
   for(struct dvm_alarm *a=vm->alarms;a;a=a->next){++count;replacement |= (token && a->token==token) || (listener && a->listener==listener);}
   if(count>=500 && !replacement){dvm__throw(vm,"java/lang/IllegalStateException","Maximum concurrent alarms reached");return false;}
   struct dvm_alarm *a=calloc(1,sizeof *a);
   if(!a){dvm__throw(vm,"java/lang/OutOfMemoryError","AlarmManager record");return false;}
   *a=(struct dvm_alarm){NULL,token,listener,handler,type,when,interval,elapsed+futurity};
   if(token)dvm_pin(vm,token);if(listener)dvm_pin(vm,listener);if(handler)dvm_pin(vm,handler);
   alarm_remove(vm,token,listener);a->next=vm->alarms;vm->alarms=a;RETV();
}
static bool alarm_cancel(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;const struct dvm_method *method=dvm__builtin_method();
   bool direct=strstr(method->sig,"OnAlarmListener;")!=NULL;
   if(!ARG(0).l) {
      if(direct || lunaria_app_target_sdk()>=24){dvm__throw(vm,"java/lang/NullPointerException","cancel called with null operation");return false;}
      RETV();
   }
   alarm_remove(vm,direct ? 0 : pending_field(vm,ARG(0).l,"token","Landroid/app/PendingIntent;"),direct ? ARG(0).l : 0);RETV();
}
static const struct rt_field rt_alarm_delivery_fields[]={{"listener","Landroid/app/AlarmManager$OnAlarmListener;"},F_END};
static const struct rt_method rt_alarm_delivery[]={M("run","()V",alarm_listener_run),M_END};
static const struct rt_method rt_alarm_listener[]={{"onAlarm","()V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},M_END};
static const struct rt_field rt_alarm_static[]={
   {"RTC_WAKEUP","I"},{"RTC","I"},{"ELAPSED_REALTIME_WAKEUP","I"},{"ELAPSED_REALTIME","I"},
   {"INTERVAL_FIFTEEN_MINUTES","J"},{"INTERVAL_HALF_HOUR","J"},{"INTERVAL_HOUR","J"},{"INTERVAL_HALF_DAY","J"},{"INTERVAL_DAY","J"},F_END
};
static const struct rt_method rt_alarm_manager[]={
   M("set","(IJLandroid/app/PendingIntent;)V",alarm_set),
   M("setExact","(IJLandroid/app/PendingIntent;)V",alarm_set),
   M("setAndAllowWhileIdle","(IJLandroid/app/PendingIntent;)V",alarm_set),
   M("setExactAndAllowWhileIdle","(IJLandroid/app/PendingIntent;)V",alarm_set),
   M("setRepeating","(IJJLandroid/app/PendingIntent;)V",alarm_set),
   M("setInexactRepeating","(IJJLandroid/app/PendingIntent;)V",alarm_set),
   M("setWindow","(IJJLandroid/app/PendingIntent;)V",alarm_set),
   M("set","(IJLjava/lang/String;Landroid/app/AlarmManager$OnAlarmListener;Landroid/os/Handler;)V",alarm_set),
   M("setExact","(IJLjava/lang/String;Landroid/app/AlarmManager$OnAlarmListener;Landroid/os/Handler;)V",alarm_set),
   M("setWindow","(IJJLjava/lang/String;Landroid/app/AlarmManager$OnAlarmListener;Landroid/os/Handler;)V",alarm_set),
   M("cancel","(Landroid/app/PendingIntent;)V",alarm_cancel),
   M("cancel","(Landroid/app/AlarmManager$OnAlarmListener;)V",alarm_cancel),
   M("canScheduleExactAlarms","()Z",alarm_can_exact),M_END
};

/* SQLiteQueryBuilder composes SQL; execution and binding belong to SQLiteDatabase. */
static bool qb_field(struct dvm *vm,dvm_ref self,const char *name,const char *type,union dvm_value *value)
{ return dvm_get_field(vm,self,name,type,value); }
static bool qb_property(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;const char *method=builtin_name();
   const char *name=!strcmp(method,"setProjectionMap") ? "projection" :
      strstr(method,"Distinct") ? "distinct" : "tables";
   const char *type=!strcmp(name,"projection") ? "Ljava/util/Map;" :
      !strcmp(name,"distinct") ? "Z" : "Ljava/lang/String;";
   if(!strncmp(method,"set",3)){dvm_set_field(vm,self,name,type,ARG(0));RETV();}
   return qb_field(vm,self,name,type,out);
}
static bool qb_append_where(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;const char *method=builtin_name();
   bool escape=!strcmp(method,"appendWhereEscapeString"),standalone=!strcmp(method,"appendWhereStandalone");
   union dvm_value previous={0},chunk={0};
   if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","where");return false;}
   if(!am_vcall(vm,ARG(0).l,"toString","()Ljava/lang/String;",NULL,0,&chunk))return false;
   qb_field(vm,self,"where","Ljava/lang/String;",&previous);
   const char *old=dvm_string_utf8(vm,previous.l),*text=dvm_string_utf8(vm,chunk.l);
   struct rt_bytes b={0};bool good=bytes_printf(&b,"%s",old ? old : "");
   if(standalone)good &= bytes_printf(&b,"%s(",old && *old ? " AND " : "");
   if(escape){
      good &= bytes_append(&b,"'",1);
      for(const char *p=text;good && p && *p;++p){good &= bytes_append(&b,p,1);if(*p=='\'')good &= bytes_append(&b,p,1);}
      good &= bytes_append(&b,"'",1);
   }else good &= bytes_printf(&b,"%s",text ? text : "");
   if(standalone)good &= bytes_append(&b,")",1);
   if(good){chunk.l=dvm_new_string_n(vm,(const char *)b.p,b.len);good=chunk.l && dvm_set_field(vm,self,"where","Ljava/lang/String;",chunk);}
   free(b.p);if(!good)return false;RETV();
}
static bool qb_limit_valid(const char *s)
{
   if(!s || !*s)return true;
   const unsigned char *p=(const unsigned char *)s;
   if(*p<'0' || *p>'9')return false;
   while(*p>='0' && *p<='9')++p;
   if(!*p)return true;
   while(*p && strchr(" \t\n\r\f\v",*p))++p;
   if(*p++!=',')return false;
   while(*p && strchr(" \t\n\r\f\v",*p))++p;
   if(*p<'0' || *p>'9')return false;
   while(*p>='0' && *p<='9')++p;
   return !*p;
}
static bool qb_build_sql(struct dvm *vm,bool distinct,dvm_ref tables,dvm_ref projection,
   const char *where,dvm_ref group,dvm_ref having,dvm_ref order,dvm_ref limit,union dvm_value *out)
{
   const char *g=dvm_string_utf8(vm,group),*h=dvm_string_utf8(vm,having),*l=dvm_string_utf8(vm,limit);
   if(h && *h && (!g || !*g)){dvm__throw(vm,"java/lang/IllegalArgumentException","HAVING requires GROUP BY");return false;}
   if(!qb_limit_valid(l)){dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid LIMIT");return false;}
   struct rt_bytes b={0};bool good=bytes_printf(&b,"SELECT %s",distinct ? "DISTINCT " : "");
   uint32_t count=projection ? dvm_array_length(vm,projection) : 0;
   if(count){
      dvm_ref *columns=dvm_array_data(vm,projection);bool first=true;
      for(uint32_t i=0;good && i<count;++i)if(columns[i]){
         good &= bytes_printf(&b,"%s%s",first ? "" : ", ",dvm_string_utf8(vm,columns[i]));first=false;
      }
   }else good &= bytes_append(&b,"*",1);
   const char *table=dvm_string_utf8(vm,tables);good &= bytes_printf(&b," FROM %s",table ? table : "null");
   const char *o=dvm_string_utf8(vm,order);
   const char *clauses[]={where,g,h,o,l},*names[]={" WHERE "," GROUP BY "," HAVING "," ORDER BY "," LIMIT "};
   for(unsigned i=0;good && i<5;++i)if(clauses[i] && *clauses[i])good &= bytes_printf(&b,"%s%s",names[i],clauses[i]);
   if(good)out->l=dvm_new_string_n(vm,(const char *)b.p,b.len);
   free(b.p);if(!good){dvm__throw(vm,"java/lang/OutOfMemoryError","SQL query");return false;}return out->l!=0;
}
static bool qb_projection(struct dvm *vm,dvm_ref mapping,dvm_ref input,dvm_ref *output)
{
   *output=input;if(!mapping)return true;
   union dvm_value r={0};
   if(input && dvm_array_length(vm,input)){
      uint32_t n=dvm_array_length(vm,input);
      *output=dvm_new_array(vm,'L',"Ljava/lang/String;",n);if(!*output)return false;
      dvm_pin(vm,*output);bool good=true;
      for(uint32_t i=0;good && i<n;++i){
         union dvm_value key={.l=((dvm_ref *)dvm_array_data(vm,input))[i]};
         good=am_vcall(vm,mapping,"get","(Ljava/lang/Object;)Ljava/lang/Object;",&key,1,&r);
         if(good && !r.l){
            const char *column=dvm_string_utf8(vm,key.l);
            if(column && (strstr(column," AS ") || strstr(column," as ")))r=key;
            else{dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid projection %s",column ? column : "null");good=false;}
         }
         if(good)((dvm_ref *)dvm_array_data(vm,*output))[i]=r.l;
      }
      dvm_unpin(vm,*output);return good;
   }
   if(!am_vcall(vm,mapping,"entrySet","()Ljava/util/Set;",NULL,0,&r))return false;
   if(!am_vcall(vm,r.l,"iterator","()Ljava/util/Iterator;",NULL,0,&r))return false;
   dvm_ref iterator=r.l;dvm_pin(vm,iterator);
   dvm_ref columns=coll_new_list(vm,"Ljava/util/ArrayList;");dvm_pin(vm,columns);
   bool good=columns!=0;
   while(good){
      good=am_vcall(vm,iterator,"hasNext","()Z",NULL,0,&r);if(!good || !r.i)break;
      good=am_vcall(vm,iterator,"next","()Ljava/lang/Object;",NULL,0,&r);if(!good)break;
      dvm_ref entry=r.l;union dvm_value key={0};
      good=am_vcall(vm,entry,"getKey","()Ljava/lang/Object;",NULL,0,&key);
      const char *name=dvm_string_utf8(vm,key.l);if(name && !strcmp(name,"_count"))continue;
      good=good && am_vcall(vm,entry,"getValue","()Ljava/lang/Object;",NULL,0,&r);
      union dvm_value ignored={0};if(good)good=list_add(vm,columns,&r,1,&ignored);
   }
   if(good){union dvm_value array={.l=dvm_new_array(vm,'L',"Ljava/lang/String;",0)};good=list_toArray(vm,columns,&array,1,&r);if(good)*output=r.l;}
   dvm_unpin(vm,columns);dvm_unpin(vm,iterator);return good;
}
static bool qb_build(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;union dvm_value tables={0},distinct={0},where={0},mapping={0};
   qb_field(vm,self,"tables","Ljava/lang/String;",&tables);qb_field(vm,self,"distinct","Z",&distinct);
   qb_field(vm,self,"where","Ljava/lang/String;",&where);qb_field(vm,self,"projection","Ljava/util/Map;",&mapping);
   dvm_ref projection=0;if(!qb_projection(vm,mapping.l,ARG(0).l,&projection))return false;
   dvm_pin(vm,projection);
   const char *fixed=dvm_string_utf8(vm,where.l),*selection=dvm_string_utf8(vm,ARG(1).l);
   struct rt_bytes combined={0};bool good=true;
   if(fixed && *fixed)good &= bytes_printf(&combined,"(%s)",fixed);
   if(selection && *selection)good &= bytes_printf(&combined,"%s(%s)",combined.len ? " AND " : "",selection);
   if(good)good=bytes_append(&combined,"",1);
   if(good)good=qb_build_sql(vm,distinct.i,tables.l,projection,(const char *)combined.p,ARG(2).l,ARG(3).l,ARG(4).l,ARG(5).l,out);
   free(combined.p);dvm_unpin(vm,projection);return good;
}
static bool qb_query(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value tables={0};qb_field(vm,self,"tables","Ljava/lang/String;",&tables);if(!tables.l)RETL(0);
   union dvm_value build[]={ARG(1),ARG(2),ARG(4),ARG(5),ARG(6),{0}};
   if(nargs>7)build[5]=ARG(7);
   union dvm_value sql={0};if(!qb_build(vm,self,build,6,&sql))return false;
   union dvm_value query[]={sql,ARG(3)};
   return am_vcall(vm,ARG(0).l,"rawQuery","(Ljava/lang/String;[Ljava/lang/String;)Landroid/database/Cursor;",query,2,out);
}
static bool qb_build_static(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;return qb_build_sql(vm,ARG(0).i,ARG(1).l,ARG(2).l,dvm_string_utf8(vm,ARG(3).l),ARG(4).l,ARG(5).l,ARG(6).l,ARG(7).l,out);
}
static const struct rt_field rt_query_builder_fields[]={
   {"tables","Ljava/lang/String;"},{"where","Ljava/lang/String;"},{"distinct","Z"},{"projection","Ljava/util/Map;"},F_END
};
static const struct rt_method rt_query_builder[]={
   M("<init>","()V",empty_void),
   M("setTables","(Ljava/lang/String;)V",qb_property),M("getTables","()Ljava/lang/String;",qb_property),
   M("setDistinct","(Z)V",qb_property),M("isDistinct","()Z",qb_property),
   M("setProjectionMap","(Ljava/util/Map;)V",qb_property),
   M("appendWhere","(Ljava/lang/CharSequence;)V",qb_append_where),
   M("appendWhereEscapeString","(Ljava/lang/String;)V",qb_append_where),
   M("appendWhereStandalone","(Ljava/lang/CharSequence;)V",qb_append_where),
   M("buildQuery","([Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",qb_build),
   SM("buildQueryString","(ZLjava/lang/String;[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",qb_build_static),
   M("query","(Landroid/database/sqlite/SQLiteDatabase;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",qb_query),
   M("query","(Landroid/database/sqlite/SQLiteDatabase;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",qb_query),M_END
};
