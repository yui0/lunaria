/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* AccountManager's authenticator side: addAccount / getAuthToken / removeAccount.
 *
 * AccountManagerService binds the Service of the package that registered the
 * account type (action android.accounts.AccountAuthenticator, the type named by
 * the accountType attribute of its meta-data xml) and asks its
 * AbstractAccountAuthenticator, through the Transport binder, to do the work.
 * The authenticator answers on the AccountAuthenticatorResponse it was given
 * -- at once, or later once an Activity it started has finished -- and that
 * completes the AccountManagerFuture the caller holds.
 *
 * An AccountManagerFuture here is a lunaria/AccountFuture: fields only, polled
 * with the interpreter lock released, since the answer may arrive from any
 * thread. The authenticator can live in another installed package; requests
 * and responses use the ordinary Binder transport in that case.
 *
 * Included once, after the helpers it uses, by dvm_runtime.c. */

static const char *const rt_iface_account_future[] = { "Landroid/accounts/AccountManagerFuture;", NULL };
static const char *const rt_iface_account_callback[] = { "Landroid/accounts/AccountManagerCallback;", NULL };
static const char *const rt_iface_service_connection[] = { "Landroid/content/ServiceConnection;", NULL };

enum { AA_ADD = 1, AA_TOKEN = 2, AA_REMOVE = 3, AA_ACCOUNTS = 4 };
enum { AA_OK = 0, AA_AUTH_ERROR, AA_CANCELED, AA_IO_ERROR };
enum { AA_ERROR_REMOTE = 1, AA_ERROR_NETWORK = 3, AA_ERROR_CANCELED = 4 };

static dvm_ref aa_bundle_new(struct dvm *vm)
{
  dvm_ref b = parcel_new_instance(vm, "Landroid/os/Bundle;");
  union dvm_value ignored = { 0 };
  return b && map_init(vm, b, NULL, 0, &ignored) ? b : 0;
}
static bool aa_bundle_string(struct dvm *vm, dvm_ref b, const char *key, const char *value)
{
  union dvm_value kv[2] = { { .l = dvm_new_string(vm, key) }, { .l = dvm_new_string(vm, value) } }, ignored = { 0 };
  return map_put(vm, b, kv, 2, &ignored);
}
static const char *aa_bundle_get_string(struct dvm *vm, dvm_ref b, const char *key)
{
  union dvm_value k = { .l = dvm_new_string(vm, key) }, v = { 0 };
  if (!b || !map_get(vm, b, &k, 1, &v) || !v.l) return NULL;
  struct dvm_object *o = dvm__obj(vm, v.l);
  return o && o->kind == DVM_OBJ_STRING ? dvm_string_utf8(vm, v.l) : NULL;
}
static bool aa_bundle_true(struct dvm *vm, dvm_ref b, const char *key)
{
  union dvm_value k = { .l = dvm_new_string(vm, key) }, v = { 0 };
  return b && map_get(vm, b, &k, 1, &v) && v.l && box_get(vm, v.l).i != 0;
}
static dvm_ref aa_bundle_get_object(struct dvm *vm, dvm_ref b, const char *key)
{
  union dvm_value k = { .l = dvm_new_string(vm, key) }, v = { 0 };
  return b && map_get(vm, b, &k, 1, &v) ? v.l : 0;
}

/* ---- the future ----------------------------------------------------------- */

static const struct rt_field rt_account_future_fields[] = {
  { "done", "Z" }, { "cancelled", "Z" }, { "kind", "I" }, { "message", "Ljava/lang/String;" },
  { "result", "Landroid/os/Bundle;" }, { "request", "I" },
  { "callback", "Landroid/accounts/AccountManagerCallback;" },
  { "handler", "Landroid/os/Handler;" }, { "activity", "Landroid/app/Activity;" },
  { "account", "Landroid/accounts/Account;" }, { "tokenType", "Ljava/lang/String;" },
  { "options", "Landroid/os/Bundle;" },
  F_END,
};
static const struct rt_field rt_account_callback_run_fields[] = {
  { "future", "Llunaria/AccountFuture;" }, F_END,
};

static int aa_int(struct dvm *vm, dvm_ref o, const char *name)
{
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, o, name, "I", &v);
  return v.i;
}
static bool aa_bool(struct dvm *vm, dvm_ref o, const char *name)
{
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, o, name, "Z", &v);
  return v.i != 0;
}
static dvm_ref aa_ref(struct dvm *vm, dvm_ref o, const char *name, const char *desc)
{
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, o, name, desc, &v);
  return v.l;
}

/* Runs the caller's callback on its handler (the main looper when none). */
static void aa_future_notify(struct dvm *vm, dvm_ref future)
{
  dvm_ref callback = aa_ref(vm, future, "callback", "Landroid/accounts/AccountManagerCallback;");
  struct dvm_class *c = dvm__class_by_desc(vm, "Llunaria/AccountCallbackRun;");
  dvm_ref job = callback && c ? dvm_new_object(vm, c) : 0;
  if (!job) return;
  union dvm_value f = { .l = future };
  (void)dvm_set_field(vm, job, "future", "Llunaria/AccountFuture;", f);
  dvm_ref handler = aa_ref(vm, future, "handler", "Landroid/os/Handler;");
  if (handler) {
    union dvm_value a = { .l = job }, posted = { 0 };
    (void)am_vcall(vm, handler, "post", "(Ljava/lang/Runnable;)Z", &a, 1, &posted);
  } else (void)queue_runnable(vm, job, false);
}

static bool aa_future_callback_run(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  dvm_ref future = aa_ref(vm, self, "future", "Llunaria/AccountFuture;");
  dvm_ref callback = future ? aa_ref(vm, future, "callback", "Landroid/accounts/AccountManagerCallback;") : 0;
  union dvm_value a = { .l = future }, ignored = { 0 };
  if (callback && !am_vcall(vm, callback, "run", "(Landroid/accounts/AccountManagerFuture;)V", &a, 1, &ignored))
    return false;
  RETV();
}

/* Completes the future once: with `result`, or with the failure `kind`. */
static void aa_future_finish(struct dvm *vm, dvm_ref future, dvm_ref result, int kind, const char *message)
{
  if (aa_bool(vm, future, "done")) return;
  union dvm_value v = { .l = result };
  (void)dvm_set_field(vm, future, "result", "Landroid/os/Bundle;", v);
  v.i = kind;
  (void)dvm_set_field(vm, future, "kind", "I", v);
  v.l = message ? dvm_new_string(vm, message) : 0;
  (void)dvm_set_field(vm, future, "message", "Ljava/lang/String;", v);
  v.i = 1;
  (void)dvm_set_field(vm, future, "done", "Z", v);
  aa_future_notify(vm, future);
}

/* The authenticator has a token for the caller: AccountManagerService keeps it
 * so that the next request for that account and type is served from the cache. */
static void aa_cache_token(struct dvm *vm, dvm_ref future, dvm_ref result)
{
  const char *token = aa_bundle_get_string(vm, result, "authtoken");
  const char *type = aa_ref(vm, future, "tokenType", "Ljava/lang/String;") ?
                     dvm_string_utf8(vm, aa_ref(vm, future, "tokenType", "Ljava/lang/String;")) : NULL;
  dvm_ref account = aa_ref(vm, future, "account", "Landroid/accounts/Account;");
  const char *name, *account_type;
  if (!token || !type || !account || !acct_of(vm, account, &name, &account_type)) {
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return;
  }
  struct acct_db db;
  if (!acct_open(&db, true)) return;
  for (size_t i = db.n; i-- > 0; )
    if (db.r[i].kind == 'T' && acct_same(&db.r[i], name, account_type) && !strcmp(db.r[i].f[2], type)) acct_drop(&db, i);
  if (acct_exists(&db, name, account_type) && acct_add(&db, 'T', name, account_type, type, token)) (void)acct_commit(&db);
  acct_close(&db);
}

/* AccountManager.AmsTask.Response.onResult */
static void aa_future_result(struct dvm *vm, dvm_ref future, dvm_ref result)
{
  dvm_ref intent = aa_bundle_get_object(vm, result, "intent");
  dvm_ref activity = aa_ref(vm, future, "activity", "Landroid/app/Activity;");
  if (getenv("LUNARIA_TRACE_ACCOUNTS")) {
    union dvm_value keys = { 0 }, text = { 0 };
    if (result && am_vcall(vm, result, "keySet", "()Ljava/util/Set;", NULL, 0, &keys) && keys.l)
      (void)am_vcall(vm, keys.l, "toString", "()Ljava/lang/String;", NULL, 0, &text);
    fprintf(stderr, "[accounts] result keys=%s intent=%u activity=%u\n", text.l ? dvm_string_utf8(vm, text.l) : "?",
            (unsigned)intent, (unsigned)activity);
    if (dvm_exception(vm)) dvm_clear_exception(vm);
  }
  if (intent && activity) {
    /* The caller gave an Activity: the intent is started for it, and the future
     * keeps waiting for the real answer. */
    union dvm_value a = { .l = intent }, ignored = { 0 };
    (void)am_vcall(vm, activity, "startActivity", "(Landroid/content/Intent;)V", &a, 1, &ignored);
    if (dvm_exception(vm)) {
      dvm_ref e = dvm_exception(vm);
      union dvm_value text = { 0 };
      dvm_pin(vm, e);
      dvm_clear_exception(vm);
      (void)am_vcall(vm, e, "getMessage", "()Ljava/lang/String;", NULL, 0, &text);
      fprintf(stderr, "[accounts] the sign-in screen cannot be started: %s\n", text.l ? dvm_string_utf8(vm, text.l) : "(no message)");
      if (getenv("LUNARIA_TRACE_ACCOUNTS")) {
        const char *rows = getenv("ANDROID_ACTIVITY_FILTERS"), *at = rows ? strstr(rows, "UNPACKING_REDIRECT") : NULL;
        if (at) {
          const char *from = at;
          while (from > rows && from[-1] != '\n') --from;
          const char *to = strchr(at, '\n');
          fprintf(stderr, "[accounts] filter row: %.*s\n", (int)(to ? to - from : strlen(from)), from);
          const char *acts = getenv("ANDROID_ACTIVITIES"), *row = acts ? strstr(acts, "UnpackingRedirectActivity") : NULL;
          if (row) {
            const char *end = strchr(row, '\n');
            fprintf(stderr, "[accounts] activity row: %.*s\n", (int)(end ? end - row : strlen(row)), row);
          } else fprintf(stderr, "[accounts] no activity row for UnpackingRedirectActivity\n");
        } else fprintf(stderr, "[accounts] no filter row names UNPACKING_REDIRECT\n");
      }
      dvm_unpin(vm, e);
      if (dvm_exception(vm)) dvm_clear_exception(vm);
    }
    return;
  }
  if (aa_int(vm, future, "request") == AA_TOKEN) aa_cache_token(vm, future, result);
  if (aa_int(vm, future, "request") == AA_REMOVE && aa_bundle_true(vm, result, "booleanResult")) {
    /* The authenticator allowed it: the account goes. */
    union dvm_value account = { .l = aa_ref(vm, future, "account", "Landroid/accounts/Account;") }, gone = { 0 };
    if (account.l) (void)accman_remove_explicit(vm, 0, &account, 1, &gone);
  }
  aa_future_finish(vm, future, result, AA_OK, NULL);
}

static void aa_future_error(struct dvm *vm, dvm_ref future, int code, const char *message)
{
  const int kind = code == AA_ERROR_CANCELED ? AA_CANCELED : code == AA_ERROR_NETWORK ? AA_IO_ERROR : AA_AUTH_ERROR;
  aa_future_finish(vm, future, 0, kind, message ? message : "");
}

/* getResult(): blocks until the answer, then returns it or throws what it was. */
static bool aa_future_wait(struct dvm *vm, dvm_ref future, long timeout_ms)
{
  long waited = 0;
  while (!aa_bool(vm, future, "done") && !aa_bool(vm, future, "cancelled")) {
    if (timeout_ms >= 0 && waited >= timeout_ms) {
      dvm__throw(vm, "android/accounts/OperationCanceledException", "timed out");
      return false;
    }
    const unsigned depth = dvm_gil_unlock_all(vm);
    usleep(2000);
    dvm_gil_relock(vm, depth);
    waited += 2;
  }
  return true;
}

static bool aa_future_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
  long timeout_ms = -1;
  if (nargs >= 2) {
    /* getResult(long, TimeUnit): convert through TimeUnit.toMillis */
    union dvm_value amount = { .j = args[0].j }, ms = { 0 };
    if (!args[1].l || !am_vcall(vm, args[1].l, "toMillis", "(J)J", &amount, 1, &ms)) return false;
    timeout_ms = (long)ms.j;
  }
  if (!aa_future_wait(vm, self, timeout_ms)) return false;
  if (aa_bool(vm, self, "cancelled")) {
    dvm__throw(vm, "android/accounts/OperationCanceledException", "");
    return false;
  }
  const int kind = aa_int(vm, self, "kind");
  dvm_ref message = aa_ref(vm, self, "message", "Ljava/lang/String;");
  const char *text = message ? dvm_string_utf8(vm, message) : "";
  if (kind == AA_CANCELED) {
    dvm__throw(vm, "android/accounts/OperationCanceledException", "%s", text);
    return false;
  }
  if (kind == AA_IO_ERROR) {
    dvm__throw(vm, "java/io/IOException", "%s", text);
    return false;
  }
  if (kind == AA_AUTH_ERROR) {
    dvm__throw(vm, "android/accounts/AuthenticatorException", "%s", text);
    return false;
  }
  dvm_ref result = aa_ref(vm, self, "result", "Landroid/os/Bundle;");
  if (aa_int(vm, self, "request") == AA_ACCOUNTS)
    RETL(aa_bundle_get_object(vm, result, "accounts"));
  if (aa_int(vm, self, "request") == AA_REMOVE) {
    const bool removed = aa_bundle_true(vm, result, "booleanResult");
    union dvm_value one = { .i = removed };
    RETL(box_make(vm, "Ljava/lang/Boolean;", one));
  }
  RETL(result);
}
static bool aa_future_isDone(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  RETI(aa_bool(vm, self, "done") || aa_bool(vm, self, "cancelled"));
}
static bool aa_future_isCancelled(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  RETI(aa_bool(vm, self, "cancelled"));
}
static bool aa_future_cancel(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  if (aa_bool(vm, self, "done")) RETI(0);
  union dvm_value one = { .i = 1 };
  (void)dvm_set_field(vm, self, "cancelled", "Z", one);
  RETI(1);
}

static const struct rt_method rt_account_future[] = {
  M("getResult", "()Ljava/lang/Object;", aa_future_get),
  M("getResult", "(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;", aa_future_get),
  M("isDone", "()Z", aa_future_isDone),
  M("isCancelled", "()Z", aa_future_isCancelled),
  M("cancel", "(Z)Z", aa_future_cancel),
  M_END,
};
static const struct rt_method rt_account_callback_run[] = { M("run", "()V", aa_future_callback_run), M_END };

/* ---- the wire between AccountManager and an authenticator -------------------
 *
 * IAccountAuthenticator (the Transport of AbstractAccountAuthenticator) and
 * IAccountAuthenticatorResponse are Binder interfaces, so the authenticator can
 * live in another process.  Transaction codes follow the AIDL order; a call
 * is  [interface token][binder response][arguments...]  and is one-way, as the
 * answer comes back later on the response, from any thread. */
#define AA_DESCRIPTOR "android.accounts.IAccountAuthenticator"
#define AA_RESPONSE_DESCRIPTOR "android.accounts.IAccountAuthenticatorResponse"
enum { AA_TX_ADD_ACCOUNT = 1, AA_TX_GET_AUTH_TOKEN = 3, AA_TX_REMOVAL_ALLOWED = 8 };
enum { AA_TX_RESULT = 1, AA_TX_CONTINUED = 2, AA_TX_ERROR = 3 };

static dvm_ref aa_parcel_new(struct dvm *vm, const char *descriptor)
{
  union dvm_value rv = { 0 };
  if (!parcel_obtain(vm, 0, NULL, 0, &rv) || !rv.l) return 0;
  struct rt_parcel *p = parcel_of(vm, rv.l);
  dvm_ref d = dvm_new_string(vm, descriptor);
  if (!p || !d || !parcel_put_i32(vm, p, 0) || !parcel_put_string(vm, p, d)) return 0;
  return rv.l;
}
/* The interface token a parcel starts with, checked. */
static bool aa_parcel_enforce(struct dvm *vm, dvm_ref data, const char *descriptor)
{
  struct rt_parcel *p = parcel_of(vm, data);
  dvm_ref token = 0;
  if (!p) return false;
  (void)parcel_take_i32(p);
  if (!parcel_take_string(vm, p, &token) || !token) return false;
  return !strcmp(dvm_string_utf8(vm, token), descriptor);
}
static bool aa_parcel_put_string(struct dvm *vm, dvm_ref parcel, const char *text)
{
  struct rt_parcel *p = parcel_of(vm, parcel);
  return p && parcel_put_string(vm, p, text ? dvm_new_string(vm, text) : 0);
}
static bool aa_parcel_put_bundle(struct dvm *vm, dvm_ref parcel, dvm_ref bundle)
{
  struct rt_parcel *p = parcel_of(vm, parcel);
  return p && parcel_put_i32(vm, p, bundle != 0) && (!bundle || parcel_write_bundle(vm, parcel, p, bundle));
}
static bool aa_parcel_get_bundle(struct dvm *vm, dvm_ref parcel, dvm_ref *bundle)
{
  struct rt_parcel *p = parcel_of(vm, parcel);
  *bundle = 0;
  return p && (!parcel_take_i32(p) || parcel_read_bundle(vm, parcel, p, bundle));
}
static const char *aa_parcel_get_string(struct dvm *vm, dvm_ref parcel)
{
  struct rt_parcel *p = parcel_of(vm, parcel);
  dvm_ref s = 0;
  return p && parcel_take_string(vm, p, &s) && s ? dvm_string_utf8(vm, s) : NULL;
}
static bool aa_transact(struct dvm *vm, dvm_ref binder, int code, dvm_ref data)
{
  union dvm_value a[4] = { { .i = code }, { .l = data }, { .l = 0 }, { .i = BINDER_FLAG_ONEWAY } }, handled = { 0 };
  const bool ok = am_vcall(vm, binder, "transact", "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z", a, 4, &handled);
  union dvm_value ignored = { 0 };
  (void)parcel_recycle(vm, data, NULL, 0, &ignored);
  return ok;
}

/* ---- the response, as the authenticator sees it ------------------------------
 * AccountAuthenticatorResponse wraps the response Binder: onResult/onError send
 * a transaction to the process that asked.  It is Parcelable (the sign-in
 * Activity is handed one in its intent), carried as that Binder. */

static const struct rt_field rt_account_response_fields[] = { { "binder", "Landroid/os/IBinder;" }, F_END };
static const struct rt_field rt_account_response_sfields[] = { { "CREATOR", "Landroid/os/Parcelable$Creator;" }, F_END };

static dvm_ref aa_response_binder(struct dvm *vm, dvm_ref self)
{
  return aa_ref(vm, self, "binder", "Landroid/os/IBinder;");
}

static bool aa_response_send(struct dvm *vm, dvm_ref self, int code, dvm_ref bundle, int error, const char *message)
{
  dvm_ref binder = aa_response_binder(vm, self);
  dvm_ref data = binder ? aa_parcel_new(vm, AA_RESPONSE_DESCRIPTOR) : 0;
  struct rt_parcel *p = data ? parcel_of(vm, data) : NULL;
  if (getenv("LUNARIA_TRACE_ACCOUNTS")) fprintf(stderr, "[accounts] response %d: binder=%u parcel=%u\n", code, (unsigned)binder, (unsigned)data);
  if (!p) {
    dvm__throw(vm, "java/lang/IllegalStateException", "the account response has no binder");
    return false;
  }
  dvm_pin(vm, data);
  bool ok = true;
  if (code == AA_TX_RESULT) ok = aa_parcel_put_bundle(vm, data, bundle);
  else if (code == AA_TX_ERROR) ok = parcel_put_i32(vm, p, error) && aa_parcel_put_string(vm, data, message);
  ok = ok && aa_transact(vm, binder, code, data);
  dvm_unpin(vm, data);
  if (!ok && getenv("LUNARIA_TRACE_ACCOUNTS")) {
    dvm_ref e = dvm_exception(vm);
    union dvm_value m = { 0 };
    dvm_pin(vm, e);
    if (e) dvm_clear_exception(vm);
    if (e) (void)am_vcall(vm, e, "toString", "()Ljava/lang/String;", NULL, 0, &m);
    fprintf(stderr, "[accounts] response %d not delivered: %s\n", code, m.l ? dvm_string_utf8(vm, m.l) : "(no exception)");
    if (e) {
      dvm_unpin(vm, e);
      vm->exception = e;
    }
  }
  return ok;
}
static bool aa_response_onResult(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)nargs;
  if (!aa_response_send(vm, self, AA_TX_RESULT, args[0].l, 0, NULL)) return false;
  RETV();
}
static bool aa_response_onRequestContinued(struct dvm *vm, dvm_ref self, const union dvm_value *args,
    int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  if (!aa_response_send(vm, self, AA_TX_CONTINUED, 0, 0, NULL)) return false;
  RETV();
}
static bool aa_response_onError(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  (void)nargs;
  if (!aa_response_send(vm, self, AA_TX_ERROR, 0, args[0].i, args[1].l ? dvm_string_utf8(vm, args[1].l) : NULL)) return false;
  RETV();
}
static bool aa_response_init_parcel(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
  (void)nargs;
  union dvm_value binder = { 0 };
  if (!parcel_readStrongBinder(vm, args[0].l, NULL, 0, &binder)) return false;
  (void)dvm_set_field(vm, self, "binder", "Landroid/os/IBinder;", binder);
  RETV();
}
static bool aa_response_writeToParcel(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                      int nargs, union dvm_value *out)
{
  (void)nargs;
  union dvm_value binder = { .l = aa_response_binder(vm, self) };
  return parcel_writeStrongBinder(vm, args[0].l, &binder, 1, out);
}
static bool aa_response_describe(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(0);
}
static const struct rt_method rt_account_response[] = {
  M("<init>", "(Landroid/os/Parcel;)V", aa_response_init_parcel),
  M("onResult", "(Landroid/os/Bundle;)V", aa_response_onResult),
  M("onRequestContinued", "()V", aa_response_onRequestContinued),
  M("onError", "(ILjava/lang/String;)V", aa_response_onError),
  M("writeToParcel", "(Landroid/os/Parcel;I)V", aa_response_writeToParcel),
  M("describeContents", "()I", aa_response_describe),
  M_END,
};

static bool aa_creator_create(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/accounts/AccountAuthenticatorResponse;");
  dvm_ref r = c ? dvm_new_object(vm, c) : 0;
  union dvm_value ignored = { 0 };
  if (!r) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "AccountAuthenticatorResponse");
    return false;
  }
  dvm_pin(vm, r);
  const bool ok = aa_response_init_parcel(vm, r, args, 1, &ignored);
  dvm_unpin(vm, r);
  if (!ok) return false;
  RETL(r);
}
static bool aa_creator_new_array(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  RETL(dvm_new_array(vm, 'L', "Landroid/accounts/AccountAuthenticatorResponse;", (uint32_t)args[0].i));
}
static const struct rt_method rt_account_response_creator[] = {
  M("createFromParcel", "(Landroid/os/Parcel;)Ljava/lang/Object;", aa_creator_create),
  M("newArray", "(I)[Ljava/lang/Object;", aa_creator_new_array),
  M_END,
};
static const char *const rt_iface_creator[] = { "Landroid/os/Parcelable$Creator;", NULL };

/* The Binder the caller gives the authenticator: answers arrive here. */
static const struct rt_field rt_account_response_binder_fields[] = {
  { "future", "Llunaria/AccountFuture;" }, F_END,
};
static bool aa_response_binder_onTransact(struct dvm *vm, dvm_ref self, const union dvm_value *args,
    int nargs, union dvm_value *out)
{
  (void)nargs;
  const int code = args[0].i;
  if (getenv("LUNARIA_TRACE_ACCOUNTS")) fprintf(stderr, "[accounts] response transaction %d\n", code);
  dvm_ref data = args[1].l, future = aa_ref(vm, self, "future", "Llunaria/AccountFuture;");
  struct rt_parcel *p = data ? parcel_of(vm, data) : NULL;
  if (!p || !future || !aa_parcel_enforce(vm, data, AA_RESPONSE_DESCRIPTOR)) RETI(0);
  if (code == AA_TX_RESULT) {
    dvm_ref bundle = 0;
    if (!aa_parcel_get_bundle(vm, data, &bundle)) return false;
    aa_future_result(vm, future, bundle);
  } else if (code == AA_TX_ERROR) {
    const int error = parcel_take_i32(p);
    const char *message = aa_parcel_get_string(vm, data);
    aa_future_error(vm, future, error, message);
  } else if (code != AA_TX_CONTINUED) RETI(0);
  RETI(1);
}
static const struct rt_method rt_account_response_binder[] = {
  M("onTransact", "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z", aa_response_binder_onTransact), M_END,
};

/* ---- AbstractAccountAuthenticator and its Transport ----------------------- */

static const struct rt_field rt_authenticator_fields[] = {
  { "context", "Landroid/content/Context;" }, { "transport", "Landroid/os/IBinder;" }, F_END,
};
static const struct rt_field rt_transport_fields[] = {
  { "auth", "Landroid/accounts/AbstractAccountAuthenticator;" }, F_END,
};

static bool aa_authenticator_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)nargs;
  union dvm_value context = { .l = args[0].l };
  (void)dvm_set_field(vm, self, "context", "Landroid/content/Context;", context);
  struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/accounts/AbstractAccountAuthenticator$Transport;");
  dvm_ref transport = c ? dvm_new_object(vm, c) : 0;
  if (!transport) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "Transport");
    return false;
  }
  union dvm_value auth = { .l = self }, t = { .l = transport }, ignored = { 0 };
  (void)binder_init(vm, transport, NULL, 0, &ignored);
  (void)dvm_set_field(vm, transport, "auth", "Landroid/accounts/AbstractAccountAuthenticator;", auth);
  (void)dvm_set_field(vm, self, "transport", "Landroid/os/IBinder;", t);
  RETV();
}
static bool aa_authenticator_getIBinder(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                        int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  RETL(aa_ref(vm, self, "transport", "Landroid/os/IBinder;"));
}
/* The default answers of the base class. */
static bool aa_authenticator_removal_allowed(struct dvm *vm, dvm_ref self, const union dvm_value *args,
    int nargs, union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  dvm_ref b = aa_bundle_new(vm);
  union dvm_value kv[2] = { { .l = dvm_new_string(vm, "booleanResult") },
    { .l = box_make(vm, "Ljava/lang/Boolean;", (union dvm_value) {
        .i = 1
      })
    }
  }, ignored = { 0 };
  if (!b || !map_put(vm, b, kv, 2, &ignored)) return false;
  RETL(b);
}
static bool aa_authenticator_null(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETL(0);
}

static const struct rt_method rt_abstract_authenticator[] = {
  M("<init>", "(Landroid/content/Context;)V", aa_authenticator_init),
  M("getIBinder", "()Landroid/os/IBinder;", aa_authenticator_getIBinder),
  M("getAccountRemovalAllowed",
  "(Landroid/accounts/AccountAuthenticatorResponse;Landroid/accounts/Account;)Landroid/os/Bundle;",
  aa_authenticator_removal_allowed),
  M("getAuthTokenLabel", "(Ljava/lang/String;)Ljava/lang/String;", aa_authenticator_null),
  M_END,
};

/* What the authenticator method raised, as the error the response reports. */
static void aa_report_exception(struct dvm *vm, dvm_ref response)
{
  dvm_ref e = dvm_exception(vm);
  struct dvm_class *c = e ? dvm_object_class(vm, e) : NULL;
  const char *name = c && c->name ? c->name : "";
  union dvm_value message = { 0 };
  dvm_pin(vm, e);
  dvm_clear_exception(vm);
  (void)am_vcall(vm, e, "getMessage", "()Ljava/lang/String;", NULL, 0, &message);
  dvm_unpin(vm, e);
  const int code = !strcmp(name, "android/accounts/NetworkErrorException") ? AA_ERROR_NETWORK : AA_ERROR_REMOTE;
  union dvm_value a[2] = { { .i = code }, { .l = message.l } }, ignored = { 0 };
  (void)am_vcall(vm, response, "onError", "(ILjava/lang/String;)V", a, 2, &ignored);
}

/* The Transport receives the call as a transaction, builds the response object
 * around the Binder the caller sent and calls the authenticator.  A Bundle it
 * returns is the answer; null means it will answer on the response later. */
static bool aa_transport_onTransact(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
  (void)nargs;
  const int code = args[0].i;
  if (getenv("LUNARIA_TRACE_ACCOUNTS")) fprintf(stderr, "[accounts] authenticator transaction %d\n", code);
  dvm_ref data = args[1].l;
  dvm_ref auth = aa_ref(vm, self, "auth", "Landroid/accounts/AbstractAccountAuthenticator;");
  struct rt_parcel *p = data ? parcel_of(vm, data) : NULL;
  if (!p || !auth || !aa_parcel_enforce(vm, data, AA_DESCRIPTOR)) RETI(0);
  struct dvm_class *rc = dvm__class_by_desc(vm, "Landroid/accounts/AccountAuthenticatorResponse;");
  dvm_ref response = rc ? dvm_new_object(vm, rc) : 0;
  union dvm_value binder = { 0 }, ignored = { 0 }, result = { 0 };
  if (!response || !parcel_readStrongBinder(vm, data, NULL, 0, &binder)) return false;
  dvm_pin(vm, response);
  (void)dvm_set_field(vm, response, "binder", "Landroid/os/IBinder;", binder);
  bool ok = true;
  if (code == AA_TX_ADD_ACCOUNT) {
    union dvm_value call[5] = { { .l = response }, { 0 }, { 0 }, { 0 }, { 0 } }, strings = { 0 };
    const char *type = aa_parcel_get_string(vm, data), *token_type = aa_parcel_get_string(vm, data);
    call[1].l = type ? dvm_new_string(vm, type) : 0;
    call[2].l = token_type ? dvm_new_string(vm, token_type) : 0;
    if (parcel_readStringArray(vm, data, NULL, 0, &strings)) call[3].l = strings.l;
    if (getenv("LUNARIA_TRACE_ACCOUNTS"))
      fprintf(stderr, "[accounts] addAccount(type=%s, tokenType=%s)\n", type ? type : "(null)", token_type ? token_type : "(null)");
    ok = aa_parcel_get_bundle(vm, data, &call[4].l) &&
         am_vcall(vm, auth, "addAccount",
                  "(Landroid/accounts/AccountAuthenticatorResponse;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Landroid/os/Bundle;)Landroid/os/Bundle;",
                  call, 5, &result);
  } else if (code == AA_TX_GET_AUTH_TOKEN) {
    union dvm_value call[4] = { { .l = response }, { 0 }, { 0 }, { 0 } };
    const char *name = aa_parcel_get_string(vm, data), *type = aa_parcel_get_string(vm, data);
    const char *token_type = aa_parcel_get_string(vm, data);
    call[1].l = name && type ? acct_new(vm, name, type) : 0;
    call[2].l = token_type ? dvm_new_string(vm, token_type) : 0;
    ok = aa_parcel_get_bundle(vm, data, &call[3].l) &&
         am_vcall(vm, auth, "getAuthToken",
                  "(Landroid/accounts/AccountAuthenticatorResponse;Landroid/accounts/Account;Ljava/lang/String;Landroid/os/Bundle;)Landroid/os/Bundle;",
                  call, 4, &result);
  } else if (code == AA_TX_REMOVAL_ALLOWED) {
    union dvm_value call[2] = { { .l = response }, { 0 } };
    const char *name = aa_parcel_get_string(vm, data), *type = aa_parcel_get_string(vm, data);
    call[1].l = name && type ? acct_new(vm, name, type) : 0;
    ok = am_vcall(vm, auth, "getAccountRemovalAllowed",
                  "(Landroid/accounts/AccountAuthenticatorResponse;Landroid/accounts/Account;)Landroid/os/Bundle;",
                  call, 2, &result);
  } else {
    dvm_unpin(vm, response);
    RETI(0);
  }
  if (getenv("LUNARIA_TRACE_ACCOUNTS"))
    fprintf(stderr, "[accounts] authenticator %d returned ok=%d result=%u response binder=%u\n", code, ok, (unsigned)result.l, (unsigned)binder.l);
  if (!ok) aa_report_exception(vm, response);
  else if (result.l) {
    union dvm_value a = { .l = result.l };
    if (getenv("LUNARIA_TRACE_ACCOUNTS")) {
      struct dvm_class *rcls = dvm_object_class(vm, response);
      fprintf(stderr, "[accounts] response object class %s, onResult %s\n", rcls && rcls->name ? rcls->name : "?",
              rcls && dvm_find_method(vm, rcls, "onResult", "(Landroid/os/Bundle;)V") ? "found" : "missing");
    }
    (void)am_vcall(vm, response, "onResult", "(Landroid/os/Bundle;)V", &a, 1, &ignored);
  }
  if (dvm_exception(vm)) {
    if (getenv("LUNARIA_TRACE_ACCOUNTS")) {
      dvm_ref e = dvm_exception(vm);
      union dvm_value m = { 0 };
      dvm_pin(vm, e);
      dvm_clear_exception(vm);
      (void)am_vcall(vm, e, "getMessage", "()Ljava/lang/String;", NULL, 0, &m);
      fprintf(stderr, "[accounts] authenticator call raised %s\n", m.l ? dvm_string_utf8(vm, m.l) : "?");
      dvm_unpin(vm, e);
    } else dvm_clear_exception(vm);
  }
  dvm_unpin(vm, response);
  RETI(1);
}
static const struct rt_method rt_authenticator_transport[] = {
  M("onTransact", "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z", aa_transport_onTransact), M_END,
};

/* ---- finding and binding the authenticator -------------------------------- */

/* The accountType a service's authenticator xml declares, or NULL. */
static const char *aa_service_account_type(struct dvm *vm, dvm_ref service_info)
{
  dvm_ref meta = aa_ref(vm, service_info, "metaData", "Landroid/os/Bundle;");
  union dvm_value key = { .l = dvm_new_string(vm, "android.accounts.AccountAuthenticator") }, id = { 0 };
  if (!meta || !bundle_getInt(vm, meta, &key, 1, &id) || !id.i) return NULL;
  union dvm_value resource = { .i = id.i }, parser = { 0 };
  if (!res_get_xml(vm, 0, &resource, 1, &parser) || !parser.l) return NULL;
  dvm_pin(vm, parser.l);
  const char *type = NULL;
  for (int guard = 0; guard < 16 && !type; ++guard) {
    union dvm_value event = { 0 };
    if (!am_vcall(vm, parser.l, "next", "()I", NULL, 0, &event) || event.i == 1) break;
    if (event.i == 2) {   /* START_TAG */
      union dvm_value a[2] = { { .l = dvm_new_string(vm, "http://schemas.android.com/apk/res/android") },
        { .l = dvm_new_string(vm, "accountType") }
      }, v = { 0 };
      if (am_vcall(vm, parser.l, "getAttributeValue", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", a, 2, &v) && v.l)
        type = dvm_string_utf8(vm, v.l);
      break;
    }
  }
  dvm_unpin(vm, parser.l);
  return type;
}

/* The intent that names the authenticator Service of `account_type`, or 0. */
static dvm_ref aa_authenticator_intent(struct dvm *vm, const char *account_type)
{
  dvm_ref query = new_intent(vm, "android.accounts.AccountAuthenticator");
  if (!query) return 0;
  dvm_pin(vm, query);
  union dvm_value a[2] = { { .l = query }, { .i = 0x80 } }, list = { 0 };
  dvm_ref found = 0;
  if (getenv("LUNARIA_TRACE_ACCOUNTS")) fprintf(stderr, "[accounts] query authenticators\n");
  if (pm_queryIntentServices(vm, 0, a, 2, &list) && list.l) {
    struct rt_list *l = list_of(vm, list.l);
    for (uint32_t i = 0; l && i < l->size && !found; ++i) {
      dvm_ref info = aa_ref(vm, l->items[i], "serviceInfo", "Landroid/content/pm/ServiceInfo;");
      const char *type = info ? aa_service_account_type(vm, info) : NULL;
      if (getenv("LUNARIA_TRACE_ACCOUNTS"))
        fprintf(stderr, "[accounts] authenticator service %s type=%s\n",
                aa_ref(vm, info, "name", "Ljava/lang/String;") ? dvm_string_utf8(vm, aa_ref(vm, info, "name", "Ljava/lang/String;")) : "?", type ? type : "(none)");
      if (!type || strcmp(type, account_type)) continue;
      dvm_ref pkg = aa_ref(vm, info, "packageName", "Ljava/lang/String;");
      dvm_ref cls = aa_ref(vm, info, "name", "Ljava/lang/String;");
      dvm_ref intent = new_intent(vm, "android.accounts.AccountAuthenticator");
      union dvm_value names[2] = { { .l = pkg }, { .l = cls } }, ignored = { 0 };
      if (intent && am_vcall(vm, intent, "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;", names, 2, &ignored))
        found = intent;
    }
  }
  /* AccountManagerService's registry belongs to the system, not to the
   * calling app. Discover authenticators from every installed package's own
   * manifest and XML resources, including non-exported system services. */
  for (size_t i = 0; !found && i < lunaria_android_package_count(); ++i) {
    struct lunaria_android_package package;
    const char *current = getenv("ANDROID_PACKAGE_NAME");
    if (!lunaria_android_package_at(i, &package) || !package.source_dir ||
        (current && !strcmp(current, package.name))) continue;
    char mapped[DVM_PATH_MAX], component[512];
    const char *apk = arm_exec_map_guest_path(package.source_dir, mapped, sizeof mapped);
    luna_file_info info;
    if (!apk || luna_file_infoat(LUNA_AT_FDCWD, apk, &info, 0) || info.size <= 22) continue;
    int matched = luna_apk_find_registered_service(apk, "android.accounts.AccountAuthenticator",
                  "android.accounts.AccountAuthenticator", "accountType", account_type,
                  (unsigned)lunaria_sdk_int(), component, sizeof component);
    if (matched != 1) continue;
    dvm_ref intent = new_intent(vm, "android.accounts.AccountAuthenticator");
    union dvm_value names[2] = {{.l = dvm_new_string(vm, package.name)},
      {.l = dvm_new_string(vm, component)}
    }, ignored = {0};
    if (intent && am_vcall(vm, intent, "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;", names, 2, &ignored))
      found = intent;
    if (getenv("LUNARIA_TRACE_ACCOUNTS"))
      fprintf(stderr, "[accounts] registered authenticator %s/%s for %s\n", package.name, component, account_type);
  }
  dvm_unpin(vm, query);
  return found;
}

/* The ServiceConnection of one request: when the authenticator is bound, the
 * request is handed to its Transport. */
static const struct rt_field rt_account_binding_fields[] = {
  { "future", "Llunaria/AccountFuture;" }, { "response", "Landroid/os/Binder;" },
  { "request", "I" }, { "account", "Landroid/accounts/Account;" },
  { "type", "Ljava/lang/String;" }, { "tokenType", "Ljava/lang/String;" },
  { "features", "[Ljava/lang/String;" }, { "options", "Landroid/os/Bundle;" }, F_END,
};

static bool aa_binding_connected(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)nargs;
  dvm_ref transport = args[1].l;
  dvm_ref future = aa_ref(vm, self, "future", "Llunaria/AccountFuture;");
  dvm_ref response = aa_ref(vm, self, "response", "Landroid/os/Binder;");
  const int request = aa_int(vm, self, "request");
  dvm_ref data = transport && future ? aa_parcel_new(vm, AA_DESCRIPTOR) : 0;
  struct rt_parcel *p = data ? parcel_of(vm, data) : NULL;
  if (!p) RETV();
  dvm_pin(vm, data);
  union dvm_value binder = { .l = response }, ignored = { 0 };
  dvm_ref account = aa_ref(vm, self, "account", "Landroid/accounts/Account;");
  const char *name = NULL, *type = NULL;
  int code = AA_TX_REMOVAL_ALLOWED;
  bool ok = parcel_writeStrongBinder(vm, data, &binder, 1, &ignored);
  if (request == AA_ADD) {
    dvm_ref t = aa_ref(vm, self, "type", "Ljava/lang/String;"), tt = aa_ref(vm, self, "tokenType", "Ljava/lang/String;");
    union dvm_value features = { .l = aa_ref(vm, self, "features", "[Ljava/lang/String;") };
    code = AA_TX_ADD_ACCOUNT;
    ok = ok && aa_parcel_put_string(vm, data, t ? dvm_string_utf8(vm, t) : NULL) &&
         aa_parcel_put_string(vm, data, tt ? dvm_string_utf8(vm, tt) : NULL) &&
         parcel_writeStringArray(vm, data, &features, 1, &ignored) &&
         aa_parcel_put_bundle(vm, data, aa_ref(vm, self, "options", "Landroid/os/Bundle;"));
  } else {
    ok = ok && acct_of(vm, account, &name, &type) &&
         aa_parcel_put_string(vm, data, name) && aa_parcel_put_string(vm, data, type);
    if (ok && request == AA_TOKEN) {
      dvm_ref tt = aa_ref(vm, self, "tokenType", "Ljava/lang/String;");
      code = AA_TX_GET_AUTH_TOKEN;
      ok = aa_parcel_put_string(vm, data, tt ? dvm_string_utf8(vm, tt) : NULL) &&
           aa_parcel_put_bundle(vm, data, aa_ref(vm, self, "options", "Landroid/os/Bundle;"));
    }
  }
  ok = ok && aa_transact(vm, transport, code, data);
  dvm_unpin(vm, data);
  if (!ok) {
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    aa_future_error(vm, future, AA_ERROR_REMOTE, "the authenticator did not accept the request");
  }
  RETV();
}
static bool aa_binding_disconnected(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETV();
}
static bool aa_binding_null(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  dvm_ref future = aa_ref(vm, self, "future", "Llunaria/AccountFuture;");
  if (future) aa_future_error(vm, future, AA_ERROR_REMOTE, "the authenticator service returned no binder");
  RETV();
}
static const struct rt_method rt_account_binding[] = {
  M("onServiceConnected", "(Landroid/content/ComponentName;Landroid/os/IBinder;)V", aa_binding_connected),
  M("onServiceDisconnected", "(Landroid/content/ComponentName;)V", aa_binding_disconnected),
  M("onNullBinding", "(Landroid/content/ComponentName;)V", aa_binding_null),
  M_END,
};

/* AccountManager copies the client's Bundle; AccountManagerService adds the
 * authenticated caller identity before invoking an authenticator. These keys
 * must be present even for overloads with no options parameter. */
static dvm_ref aa_login_options(struct dvm *vm, dvm_ref source, bool notify)
{
  dvm_ref result = aa_bundle_new(vm);
  if (!result) return 0;
  dvm_pin(vm, result);
  union dvm_value a[2] = {{.l = source}}, ignored = {0};
  bool good = !source || map_putAll(vm, result, a, 1, &ignored);
  const char *package = getenv("ANDROID_PACKAGE_NAME");
  good = good && aa_bundle_string(vm, result, "androidPackageName", package ? package : "");
  a[0].l = dvm_new_string(vm, "callerUid");
  a[1].i = arm_exec_guest_uid();
  good = good && bundle_putInt(vm, result, a, 2, &ignored);
  a[0].l = dvm_new_string(vm, "callerPid");
  a[1].i = arm_exec_guest_pid();
  good = good && bundle_putInt(vm, result, a, 2, &ignored);
  if (notify) {
    a[0].l = dvm_new_string(vm, "notifyOnAuthFailure");
    a[1].i = 1;
    good = good && bundle_putBoolean(vm, result, a, 2, &ignored);
  }
  dvm_unpin(vm, result);
  return good && !dvm_exception(vm) ? result : 0;
}

/* Starts one request and returns its future (never null; failures complete it). */
static dvm_ref aa_start(struct dvm *vm, int request, const char *type, dvm_ref account, dvm_ref token_type,
                        dvm_ref features, dvm_ref options, dvm_ref activity, dvm_ref callback, dvm_ref handler, bool notify)
{
  struct dvm_class *fc = dvm__class_by_desc(vm, "Llunaria/AccountFuture;");
  struct dvm_class *rc = dvm__class_by_desc(vm, "Llunaria/AccountResponseBinder;");
  struct dvm_class *bc = dvm__class_by_desc(vm, "Llunaria/AccountBinding;");
  dvm_ref future = fc ? dvm_new_object(vm, fc) : 0;
  if (!future) return 0;
  dvm_pin(vm, future);
  if (request == AA_ADD || request == AA_TOKEN) {
    options = aa_login_options(vm, options, notify);
    if (!options) {
      dvm_unpin(vm, future);
      return 0;
    }
    (void)dvm_set_field(vm, future, "options", "Landroid/os/Bundle;", (union dvm_value) {
      .l = options
    });
  }
  union dvm_value v = { .i = request };
  (void)dvm_set_field(vm, future, "request", "I", v);
  v.l = callback;
  (void)dvm_set_field(vm, future, "callback", "Landroid/accounts/AccountManagerCallback;", v);
  v.l = handler;
  (void)dvm_set_field(vm, future, "handler", "Landroid/os/Handler;", v);
  v.l = activity;
  (void)dvm_set_field(vm, future, "activity", "Landroid/app/Activity;", v);
  v.l = account;
  (void)dvm_set_field(vm, future, "account", "Landroid/accounts/Account;", v);
  v.l = token_type;
  (void)dvm_set_field(vm, future, "tokenType", "Ljava/lang/String;", v);

  dvm_ref intent = type ? aa_authenticator_intent(vm, type) : 0;
  dvm_ref response = rc ? dvm_new_object(vm, rc) : 0;
  dvm_ref binding = bc ? dvm_new_object(vm, bc) : 0;
  if (!intent || !response || !binding)
    fprintf(stderr, "[accounts] no authenticator for %s (intent=%u response=%u binding=%u)\n",
            type ? type : "?", (unsigned)intent, (unsigned)response, (unsigned)binding);
  if (!intent || !response || !binding) {
    aa_future_error(vm, future, AA_ERROR_REMOTE, "bind failed: no authenticator for this account type");
    dvm_unpin(vm, future);
    return future;
  }
  v.l = future;
  (void)dvm_set_field(vm, response, "future", "Llunaria/AccountFuture;", v);
  {
    union dvm_value ignored = { 0 };
    (void)binder_init(vm, response, NULL, 0, &ignored);
  }
  (void)dvm_set_field(vm, binding, "future", "Llunaria/AccountFuture;", v);
  v.l = response;
  (void)dvm_set_field(vm, binding, "response", "Landroid/os/Binder;", v);
  v.i = request;
  (void)dvm_set_field(vm, binding, "request", "I", v);
  v.l = account;
  (void)dvm_set_field(vm, binding, "account", "Landroid/accounts/Account;", v);
  v.l = type ? dvm_new_string(vm, type) : 0;
  (void)dvm_set_field(vm, binding, "type", "Ljava/lang/String;", v);
  v.l = token_type;
  (void)dvm_set_field(vm, binding, "tokenType", "Ljava/lang/String;", v);
  v.l = features;
  (void)dvm_set_field(vm, binding, "features", "[Ljava/lang/String;", v);
  v.l = options;
  (void)dvm_set_field(vm, binding, "options", "Landroid/os/Bundle;", v);
  dvm_pin(vm, binding);
  dvm_pin(vm, response);   /* held while the bind is in flight */
  union dvm_value bind[3] = { { .l = intent }, { .l = binding }, { .i = 1 /* BIND_AUTO_CREATE */ } }, bound = { 0 };
  if (!ctx_bindService(vm, 0, bind, 3, &bound) || !bound.i)
    fprintf(stderr, "[accounts] bindService of the authenticator failed\n");
  if (!bound.i) {
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    aa_future_error(vm, future, AA_ERROR_REMOTE, "bind failed: the authenticator service cannot be bound");
  }
  dvm_unpin(vm, future);
  return future;
}

/* ---- AccountManager entry points ------------------------------------------ */

static bool accman_addAccount(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  if (!args[0].l) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "accountType is null");
    return false;
  }
  RETL(aa_start(vm, AA_ADD, dvm_string_utf8(vm, args[0].l), 0, args[1].l, args[2].l, args[3].l, args[4].l, args[5].l, args[6].l, false));
}

/* A token already held for the account is answered without the authenticator. */
static dvm_ref aa_cached_token_result(struct dvm *vm, dvm_ref account, dvm_ref token_type)
{
  const char *name, *type;
  if (!token_type || !acct_of(vm, account, &name, &type)) return 0;
  union dvm_value args[2] = { { .l = account }, { .l = token_type } }, token = { 0 };
  if (!accman_value(vm, args, 'T', &token) || !token.l) return 0;
  dvm_ref b = aa_bundle_new(vm);
  if (!b) return 0;
  dvm_pin(vm, b);
  (void)(aa_bundle_string(vm, b, "authAccount", name) && aa_bundle_string(vm, b, "accountType", type) &&
         aa_bundle_string(vm, b, "authtoken", dvm_string_utf8(vm, token.l)));
  dvm_unpin(vm, b);
  return b;
}

static bool accman_getAuthToken(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  /* (Account, String, Bundle, Activity|boolean, callback, handler) */
  const char *name, *type;
  if (!acct_of(vm, args[0].l, &name, &type)) return false;
  bool with_activity = !strcmp(dvm__builtin_method()->sig,
                               "(Landroid/accounts/Account;Ljava/lang/String;Landroid/os/Bundle;Landroid/app/Activity;Landroid/accounts/AccountManagerCallback;Landroid/os/Handler;)Landroid/accounts/AccountManagerFuture;");
  dvm_ref activity = with_activity ? args[3].l : 0;
  dvm_ref cached = aa_cached_token_result(vm, args[0].l, args[1].l);
  if (cached) {
    struct dvm_class *fc = dvm__class_by_desc(vm, "Llunaria/AccountFuture;");
    dvm_ref future = fc ? dvm_new_object(vm, fc) : 0;
    if (!future) RETL(0);
    union dvm_value v = { .l = args[4].l };
    (void)dvm_set_field(vm, future, "callback", "Landroid/accounts/AccountManagerCallback;", v);
    v.l = args[5].l;
    (void)dvm_set_field(vm, future, "handler", "Landroid/os/Handler;", v);
    aa_future_finish(vm, future, cached, AA_OK, NULL);
    RETL(future);
  }
  RETL(aa_start(vm, AA_TOKEN, type, args[0].l, args[1].l, 0, args[2].l, activity, args[4].l, args[5].l, !with_activity && args[3].i));
}

/* blockingGetAuthToken(account, type, notifyAuthFailure): the token, or null. */
static bool accman_blockingGetAuthToken(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                        int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const char *name, *type;
  if (!acct_of(vm, args[0].l, &name, &type)) return false;
  dvm_ref cached = aa_cached_token_result(vm, args[0].l, args[1].l);
  if (cached) RETL(dvm_new_string(vm, aa_bundle_get_string(vm, cached, "authtoken")));
  dvm_ref future = aa_start(vm, AA_TOKEN, type, args[0].l, args[1].l, 0, 0, 0, 0, 0, args[2].i != 0);
  union dvm_value result = { 0 };
  if (!future) RETL(0);
  dvm_pin(vm, future);
  const bool ok = aa_future_get(vm, future, NULL, 0, &result);
  dvm_unpin(vm, future);
  if (!ok) return false; /* The blocking API propagates the declared exceptions. */
  const char *token = aa_bundle_get_string(vm, result.l, "authtoken");
  RETL(token ? dvm_new_string(vm, token) : 0);
}

static bool accman_removeAccount(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const char *name, *type;
  if (!acct_of(vm, args[0].l, &name, &type)) return false;
  const bool with_activity = !strcmp(dvm__builtin_method()->sig,
                                     "(Landroid/accounts/Account;Landroid/app/Activity;Landroid/accounts/AccountManagerCallback;Landroid/os/Handler;)Landroid/accounts/AccountManagerFuture;");
  dvm_ref activity = with_activity ? args[1].l : 0;
  dvm_ref callback = with_activity ? args[2].l : args[1].l;
  dvm_ref handler = with_activity ? args[3].l : args[2].l;
  /* The authenticator may veto; the account goes only if it does not. */
  RETL(aa_start(vm, AA_REMOVE, type, args[0].l, 0, 0, 0, activity, callback, handler, false));
}

/* setPassword(account, password): the stored password; null clears it. */
static bool x_accman_setPassword(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const char *name, *type;
  if (!acct_of(vm, args[0].l, &name, &type)) return false;
  const char *password = args[1].l ? dvm_string_utf8(vm, args[1].l) : "";
  struct acct_db db;
  if (!acct_open(&db, true)) RETV();
  for (size_t i = 0; i < db.n; ++i) {
    if (db.r[i].kind != 'A' || !acct_same(&db.r[i], name, type)) continue;
    acct_drop(&db, i);
    (void)acct_add(&db, 'A', name, type, password, NULL);
    (void)acct_commit(&db);
    break;
  }
  acct_close(&db);
  RETV();
}

/* getAccountsByTypeAndFeatures(type, features, callback, handler): the accounts
 * of that type (this device's accounts have no feature the caller could ask
 * for, so a non-empty feature list matches none), delivered as the
 * "accounts" array of the future's result bundle. */
static bool x_accman_accounts_by_features(struct dvm *vm, dvm_ref self, const union dvm_value *args,
    int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const char *type = args[0].l ? dvm_string_utf8(vm, args[0].l) : NULL;
  const bool features = args[1].l && dvm_array_length(vm, args[1].l) > 0;
  struct dvm_class *fc = dvm__class_by_desc(vm, "Llunaria/AccountFuture;");
  dvm_ref future = fc ? dvm_new_object(vm, fc) : 0;
  dvm_ref bundle = aa_bundle_new(vm);
  if (!future || !bundle) RETL(0);
  dvm_pin(vm, future);
  dvm_pin(vm, bundle);
  union dvm_value list = { .l = features ? dvm_new_array(vm, 'L', "Landroid/accounts/Account;", 0) : accman_list(vm, type) };
  union dvm_value kv[2] = { { .l = dvm_new_string(vm, "accounts") }, list }, ignored = { 0 };
  (void)map_put(vm, bundle, kv, 2, &ignored);
  union dvm_value v = { .l = args[2].l };
  (void)dvm_set_field(vm, future, "callback", "Landroid/accounts/AccountManagerCallback;", v);
  v.l = args[3].l;
  (void)dvm_set_field(vm, future, "handler", "Landroid/os/Handler;", v);
  (void)dvm_set_field(vm, future, "request", "I", (union dvm_value) {
    .i = AA_ACCOUNTS
  });
  aa_future_finish(vm, future, bundle, AA_OK, NULL);
  dvm_unpin(vm, bundle);
  dvm_unpin(vm, future);
  RETL(future);
}

/* getAuthToken(account, type, notifyAuthFailure, callback, handler): the old form,
 * the same request with no options and no Activity to start. */
static bool x_accman_getAuthToken_legacy(struct dvm *vm, dvm_ref self, const union dvm_value *args,
    int nargs, union dvm_value *out)
{
  (void)nargs;
  const char *name, *type;
  if (!acct_of(vm, args[0].l, &name, &type)) return false;
  dvm_ref cached = aa_cached_token_result(vm, args[0].l, args[1].l);
  if (cached) {
    struct dvm_class *fc = dvm__class_by_desc(vm, "Llunaria/AccountFuture;");
    dvm_ref future = fc ? dvm_new_object(vm, fc) : 0;
    if (!future) RETL(0);
    union dvm_value v = { .l = args[3].l };
    (void)dvm_set_field(vm, future, "callback", "Landroid/accounts/AccountManagerCallback;", v);
    v.l = args[4].l;
    (void)dvm_set_field(vm, future, "handler", "Landroid/os/Handler;", v);
    aa_future_finish(vm, future, cached, AA_OK, NULL);
    RETL(future);
  }
  (void)self;
  RETL(aa_start(vm, AA_TOKEN, type, args[0].l, args[1].l, 0, 0, 0, args[3].l, args[4].l, args[2].i != 0));
}

static const struct rt_method rt_account_manager[] = {
  SM("get", "(Landroid/content/Context;)Landroid/accounts/AccountManager;", accman_get),
  M("getAccounts", "()[Landroid/accounts/Account;", accman_accounts),
  M("getAccountsByType", "(Ljava/lang/String;)[Landroid/accounts/Account;", accman_accounts),
  M("addOnAccountsUpdatedListener",
  "(Landroid/accounts/OnAccountsUpdateListener;Landroid/os/Handler;Z)V", accman_add_listener),
  M("addOnAccountsUpdatedListener",
  "(Landroid/accounts/OnAccountsUpdateListener;Landroid/os/Handler;Z[Ljava/lang/String;)V", accman_add_listener),
  M("removeOnAccountsUpdatedListener",
  "(Landroid/accounts/OnAccountsUpdateListener;)V", accman_remove_listener),
  M("addAccountExplicitly", "(Landroid/accounts/Account;Ljava/lang/String;Landroid/os/Bundle;)Z", accman_add_explicit),
  /* ...and the package-visibility map: every package sees every account on this single-user device. */
  M("addAccountExplicitly", "(Landroid/accounts/Account;Ljava/lang/String;Landroid/os/Bundle;Ljava/util/Map;)Z", accman_add_explicit),
  M("removeAccountExplicitly", "(Landroid/accounts/Account;)Z", accman_remove_explicit),
  M("getUserData", "(Landroid/accounts/Account;Ljava/lang/String;)Ljava/lang/String;", accman_getUserData),
  M("setUserData", "(Landroid/accounts/Account;Ljava/lang/String;Ljava/lang/String;)V", accman_setUserData),
  M("peekAuthToken", "(Landroid/accounts/Account;Ljava/lang/String;)Ljava/lang/String;", accman_peekAuthToken),
  M("setAuthToken", "(Landroid/accounts/Account;Ljava/lang/String;Ljava/lang/String;)V", accman_setAuthToken),
  M("getPassword", "(Landroid/accounts/Account;)Ljava/lang/String;", accman_getPassword),
  M("setPassword", "(Landroid/accounts/Account;Ljava/lang/String;)V", x_accman_setPassword),
  M("getAccountsByTypeAndFeatures",
  "(Ljava/lang/String;[Ljava/lang/String;Landroid/accounts/AccountManagerCallback;Landroid/os/Handler;)Landroid/accounts/AccountManagerFuture;",
  x_accman_accounts_by_features),
  M("invalidateAuthToken", "(Ljava/lang/String;Ljava/lang/String;)V", accman_invalidate),
  M("addAccount",
  "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Landroid/os/Bundle;Landroid/app/Activity;Landroid/accounts/AccountManagerCallback;Landroid/os/Handler;)Landroid/accounts/AccountManagerFuture;",
  accman_addAccount),
  M("getAuthToken",
  "(Landroid/accounts/Account;Ljava/lang/String;Landroid/os/Bundle;Landroid/app/Activity;Landroid/accounts/AccountManagerCallback;Landroid/os/Handler;)Landroid/accounts/AccountManagerFuture;",
  accman_getAuthToken),
  M("getAuthToken",
  "(Landroid/accounts/Account;Ljava/lang/String;Landroid/os/Bundle;ZLandroid/accounts/AccountManagerCallback;Landroid/os/Handler;)Landroid/accounts/AccountManagerFuture;",
  accman_getAuthToken),
  M("getAuthToken",
  "(Landroid/accounts/Account;Ljava/lang/String;ZLandroid/accounts/AccountManagerCallback;Landroid/os/Handler;)Landroid/accounts/AccountManagerFuture;",
  x_accman_getAuthToken_legacy),
  M("blockingGetAuthToken", "(Landroid/accounts/Account;Ljava/lang/String;Z)Ljava/lang/String;", accman_blockingGetAuthToken),
  M("removeAccount",
  "(Landroid/accounts/Account;Landroid/accounts/AccountManagerCallback;Landroid/os/Handler;)Landroid/accounts/AccountManagerFuture;",
  accman_removeAccount),
  M("removeAccount",
  "(Landroid/accounts/Account;Landroid/app/Activity;Landroid/accounts/AccountManagerCallback;Landroid/os/Handler;)Landroid/accounts/AccountManagerFuture;",
  accman_removeAccount),
  M_END,
};

/* ---- the device's "Add account" screen -------------------------------------
 *
 * Settings > Accounts > Add account asks AccountManager for the type's
 * authenticator and starts the Activity it answers with.  Launched as the
 * main component of the package that owns the authenticator
 * (LUNARIA_LAUNCH_ACTIVITY=com.android.settings.accounts.AddAccountSettings), it does the same for
 * the account type in the intent ("accountType", default com.google) and
 * finishes when the account has been added or the attempt has ended. */

static const struct rt_field rt_add_account_done_fields[] = {
  { "activity", "Landroid/app/Activity;" }, F_END,
};

static bool aa_add_done_run(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)nargs;
  dvm_ref activity = aa_ref(vm, self, "activity", "Landroid/app/Activity;");
  union dvm_value result = { 0 }, ignored = { 0 };
  const bool ok = aa_future_get(vm, args[0].l, NULL, 0, &result);
  if (!ok) {
    dvm_ref e = dvm_exception(vm);
    union dvm_value message = { 0 };
    if (e) (void)am_vcall(vm, e, "getMessage", "()Ljava/lang/String;", NULL, 0, &message);
    fprintf(stderr, "[accounts] add account failed: %s\n", message.l ? dvm_string_utf8(vm, message.l) : "?");
    dvm_clear_exception(vm);
  } else {
    fprintf(stderr, "[accounts] add account finished: %s\n",
            aa_bundle_get_string(vm, result.l, "authAccount") ? aa_bundle_get_string(vm, result.l, "authAccount") : "(no account name)");
  }
  if (activity) (void)am_vcall(vm, activity, "finish", "()V", NULL, 0, &ignored);
  RETV();
}
static const struct rt_method rt_add_account_done[] = {
  M("run", "(Landroid/accounts/AccountManagerFuture;)V", aa_add_done_run), M_END,
};

static bool aa_add_account_onCreate(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value intent = { 0 }, key = { .l = dvm_new_string(vm, "accountType") }, type = { 0 };
  if (!am_vcall(vm, self, "getIntent", "()Landroid/content/Intent;", NULL, 0, &intent)) return false;
  if (intent.l) (void)am_vcall(vm, intent.l, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;", &key, 1, &type);
  if (dvm_exception(vm)) dvm_clear_exception(vm);
  const char *want = type.l ? dvm_string_utf8(vm, type.l) : "com.google";
  struct dvm_class *dc = dvm__class_by_desc(vm, "Llunaria/AddAccountDone;");
  dvm_ref done = dc ? dvm_new_object(vm, dc) : 0;
  if (!done) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "AddAccountDone");
    return false;
  }
  union dvm_value a = { .l = self };
  (void)dvm_set_field(vm, done, "activity", "Landroid/app/Activity;", a);
  union dvm_value call[7] = { { .l = dvm_new_string(vm, want) }, { 0 }, { 0 }, { 0 }, { .l = self }, { .l = done }, { 0 } }, future = { 0 };
  if (!accman_addAccount(vm, 0, call, 7, &future)) return false;
  fprintf(stderr, "[accounts] adding an account of type %s\n", want);
  RETV();
}
static const struct rt_method rt_add_account_activity[] = {
  M("onCreate", "(Landroid/os/Bundle;)V", aa_add_account_onCreate), M_END,
};


/* ---- ContentResolver sync settings -------------------------------------------
 *
 * Whether an account syncs an authority, and whether automatically, is state
 * of the device's SyncManager; it lives with the accounts, in the same shared
 * store, as 'Y name type authority syncable auto'.  There is no sync adapter
 * runner here: requestSync() is accepted and nothing is scheduled, which is
 * what a device with every adapter unavailable does. */
static bool sync_find(struct acct_db *db, const char *name, const char *type, const char *authority, size_t *at)
{
  for (size_t i = 0; i < db->n; ++i)
    if (db->r[i].kind == 'Y' && acct_same(&db->r[i], name, type) && !strcmp(db->r[i].f[2], authority)) {
      *at = i;
      return true;
    }
  return false;
}
/* "syncable,auto" of an (account, authority); defaults -1 and false. */
static void sync_read(struct dvm *vm, const union dvm_value *args, int *syncable, bool *automatic)
{
  const char *name, *type;
  *syncable = -1;
  *automatic = false;
  if (!args[1].l || !acct_of(vm, args[0].l, &name, &type)) {
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return;
  }
  const char *authority = dvm_string_utf8(vm, args[1].l);
  struct acct_db db;
  size_t at;
  if (!authority || !acct_open(&db, false)) return;
  if (sync_find(&db, name, type, authority, &at)) sscanf(db.r[at].f[3], "%d,%d", syncable, (int[]) {
    0
  }), *automatic = strchr(db.r[at].f[3], ',') && atoi(strchr(db.r[at].f[3], ',') + 1);
  acct_close(&db);
}
static bool sync_write(struct dvm *vm, const union dvm_value *args, int syncable, bool automatic)
{
  const char *name, *type;
  if (!args[1].l) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "authority must not be empty");
    return false;
  }
  if (!acct_of(vm, args[0].l, &name, &type)) return false;
  const char *authority = dvm_string_utf8(vm, args[1].l);
  struct acct_db db;
  size_t at;
  char value[32];
  if (!authority || !acct_open(&db, true)) return true;
  snprintf(value, sizeof value, "%d,%d", syncable, automatic ? 1 : 0);
  if (sync_find(&db, name, type, authority, &at)) acct_drop(&db, at);
  (void)acct_add(&db, 'Y', name, type, authority, value);
  (void)acct_commit(&db);
  acct_close(&db);
  return true;
}
static bool x_sync_setIsSyncable(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  int syncable;
  bool automatic;
  (void)self;
  (void)nargs;
  sync_read(vm, args, &syncable, &automatic);
  if (!sync_write(vm, args, args[2].i > 0 ? 1 : args[2].i < 0 ? -1 : 0, automatic)) return false;
  RETV();
}
static bool x_sync_setSyncAutomatically(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  int syncable;
  bool automatic;
  (void)self;
  (void)nargs;
  sync_read(vm, args, &syncable, &automatic);
  if (!sync_write(vm, args, syncable, args[2].i != 0)) return false;
  RETV();
}
static bool x_sync_getIsSyncable(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  int syncable;
  bool automatic;
  (void)self;
  (void)nargs;
  sync_read(vm, args, &syncable, &automatic);
  RETI(syncable);
}
static bool x_sync_getSyncAutomatically(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  int syncable;
  bool automatic;
  (void)self;
  (void)nargs;
  sync_read(vm, args, &syncable, &automatic);
  RETI(automatic);
}
static bool x_sync_master(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(1);
}
static bool x_sync_none(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(0);
}
static bool x_sync_empty_list(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  RETL(coll_new_list(vm, "Ljava/util/ArrayList;"));
}
static const struct rt_method rt_x_content_resolver_sync[] = {
  SM("setIsSyncable", "(Landroid/accounts/Account;Ljava/lang/String;I)V", x_sync_setIsSyncable),
  SM("getIsSyncable", "(Landroid/accounts/Account;Ljava/lang/String;)I", x_sync_getIsSyncable),
  SM("setSyncAutomatically", "(Landroid/accounts/Account;Ljava/lang/String;Z)V", x_sync_setSyncAutomatically),
  SM("getSyncAutomatically", "(Landroid/accounts/Account;Ljava/lang/String;)Z", x_sync_getSyncAutomatically),
  SM("getMasterSyncAutomatically", "()Z", x_sync_master),
  SM("setMasterSyncAutomatically", "(Z)V", empty_void),
  SM("requestSync", "(Landroid/accounts/Account;Ljava/lang/String;Landroid/os/Bundle;)V", empty_void),
  SM("cancelSync", "(Landroid/accounts/Account;Ljava/lang/String;)V", empty_void),
  SM("addPeriodicSync", "(Landroid/accounts/Account;Ljava/lang/String;Landroid/os/Bundle;J)V", empty_void),
  SM("removePeriodicSync", "(Landroid/accounts/Account;Ljava/lang/String;Landroid/os/Bundle;)V", empty_void),
  SM("isSyncActive", "(Landroid/accounts/Account;Ljava/lang/String;)Z", x_sync_none),
  SM("isSyncPending", "(Landroid/accounts/Account;Ljava/lang/String;)Z", x_sync_none),
  SM("getCurrentSyncs", "()Ljava/util/List;", x_sync_empty_list),
  SM("getPeriodicSyncs", "(Landroid/accounts/Account;Ljava/lang/String;)Ljava/util/List;", x_sync_empty_list),
  M_END,
};

/* MPL-2.0. Account's Parcelable reader, used inside GMS TokenResponse. */
static bool account_init_parcel(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  struct rt_parcel *p=args[0].l?parcel_of(vm,args[0].l):NULL;
  if(!p) {
    dvm__throw(vm,"java/lang/NullPointerException","Parcel");
    return false;
  }
  dvm_pin(vm,self);
  union dvm_value values[3]= {{0},{0},{0}},ignored= {0};
  bool ok=parcel_take_string(vm,p,&values[0].l);
  if(ok)dvm_set_field(vm,self,"name","Ljava/lang/String;",values[0]);
  if(ok)ok=parcel_take_string(vm,p,&values[1].l);
  if(ok)dvm_set_field(vm,self,"type","Ljava/lang/String;",values[1]);
  if(ok)ok=parcel_take_string(vm,p,&values[2].l);
  if(ok) {
    dvm_set_field(vm,self,"accessId","Ljava/lang/String;",values[2]);
    ok=account_init(vm,self,values,2,&ignored);
  }
  dvm_unpin(vm,self);
  if(!ok)return false;
  RETV();
}
static bool account_creator_create(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)self;
  struct dvm_class *c=dvm__class_by_desc(vm,"Landroid/accounts/Account;");
  dvm_ref account=c?dvm_new_object(vm,c):0;
  if(!account)return false;
  union dvm_value ignored= {0};
  if(!account_init_parcel(vm,account,args,nargs,&ignored))return false;
  RETL(account);
}
static bool account_creator_array(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)self;
  (void)nargs;
  if(args[0].i<0) {
    dvm__throw(vm,"java/lang/NegativeArraySizeException","Account[]");
    return false;
  }
  RETL(dvm_new_array(vm,'L',"Landroid/accounts/Account;",args[0].i));
}
static bool account_get_access_id(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value id= {0};
  dvm_get_field(vm,self,"accessId","Ljava/lang/String;",&id);
  RETL(id.l);
}
static const struct rt_method rt_account_parcel_more[]= {
  M("<init>","(Landroid/os/Parcel;)V",account_init_parcel),
  M("getAccessId","()Ljava/lang/String;",account_get_access_id),M_END
};
static const struct rt_method rt_account_creator[]= {
  M("createFromParcel","(Landroid/os/Parcel;)Ljava/lang/Object;",account_creator_create),
  M("newArray","(I)[Ljava/lang/Object;",account_creator_array),M_END
};
