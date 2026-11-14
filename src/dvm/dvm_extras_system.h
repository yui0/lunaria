/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * SPDX-License-Identifier: MPL-2.0
 */
/* Framework system services that had no implementation: the calling identity,
 * telephony identifiers, event/log sinks, device config, and the small queries
 * a system app (Play services, the Play Store) asks at start-up.  A phone with
 * no SIM, one user, and no work profile answers each of them. */

/* --- Binder calling identity --------------------------------------------- *
 * clearCallingIdentity() returns a token that restoreCallingIdentity() puts back:
 * the caller's uid and pid packed in one long.  Cleared, the thread's identity is
 * the process's own (zero is "not inside a transaction"). */
static bool binder_clear_identity(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  const int64_t token = (int64_t)(((uint64_t)g_binder_caller_uid << 32) | g_binder_caller_pid);
  g_binder_caller_uid = g_binder_caller_pid = 0;
  RETJ(token);
}

static bool binder_restore_identity(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)nargs;
  (void)out;
  g_binder_caller_uid = (uint32_t)((uint64_t)ARG(0).j >> 32);
  g_binder_caller_pid = (uint32_t)((uint64_t)ARG(0).j & 0xffffffffu);
  RETV();
}

static const struct rt_method rt_x_binder_identity[] = {
  SM("clearCallingIdentity", "()J", binder_clear_identity),
  SM("restoreCallingIdentity", "(J)V", binder_restore_identity),
  /* Commands are sent as they are made; there is nothing queued to flush. */
  SM("flushPendingCommands", "()V", empty_void),
  M_END,
};

/* --- Process / Os --------------------------------------------------------- */
static bool x_process_user_handle(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  RETL(user_handle_of(vm, 0));
}

static bool x_os_getuid(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI((int32_t)arm_exec_guest_uid());
}

/* prctl() on a guest thread: option names (PR_SET_NAME and the like) succeed. */
static bool x_os_prctl(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(0);
}

static const struct rt_method rt_x_process_more[] = {
  SM("myUserHandle", "()Landroid/os/UserHandle;", x_process_user_handle),
  M_END,
};
static const struct rt_method rt_x_os_more[] = {
  SM("getuid", "()I", x_os_getuid),
  SM("prctl", "(IJJJJ)I", x_os_prctl),
  M_END,
};

/* --- UserManager: one full user, no profiles ------------------------------ */
static bool x_um_is_user_of_type(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const char *type = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
  RETI(type && !strcmp(type, "android.os.usertype.full.SYSTEM"));
}

static bool x_um_for_serial(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  RETL(ARG(0).j == 0 ? user_handle_of(vm, 0) : 0);
}

static bool x_um_no_profile_parent(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETL(0);
}

static bool x_null_ref(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETL(0);
}
static const struct rt_method rt_x_user_manager[] = {
  /* Both of these are static queries about the device, not about a manager. */
  SM("isHeadlessSystemUserMode", "()Z", ret_false),
  SM("supportsMultipleUsers", "()Z", ret_false),
  M("supportsMultipleUsers", "()Z", ret_false),
  /* No seed account was provisioned for this user. */
  M("getSeedAccountOptions", "()Landroid/os/PersistableBundle;", x_null_ref),
  M("getSeedAccountName", "()Ljava/lang/String;", x_null_ref),
  M("getSeedAccountType", "()Ljava/lang/String;", x_null_ref),
  M("isCloneProfile", "()Z", ret_false),
  M("isManagedProfile", "()Z", ret_false),
  M("isUserOfType", "(Ljava/lang/String;)Z", x_um_is_user_of_type),
  M("getUserForSerialNumber", "(J)Landroid/os/UserHandle;", x_um_for_serial),
  /* The hidden getUserSerialNumber(int userId) is an int; the public
   * getSerialNumberForUser(UserHandle) is a long.  The one user is serial 0. */
  M("getUserSerialNumber", "(I)I", ret_zero),
  M("getSerialNumberForUser", "(Landroid/os/UserHandle;)J", um_zero_long),
  M("getProfileParent", "(I)Landroid/content/pm/UserInfo;", x_um_no_profile_parent),
  M_END,
};

/* --- TelephonyManager: no SIM --------------------------------------------- *
 * Identifiers a SIM or radio would give are absent (null), the carrier is
 * UNKNOWN_CARRIER_ID (-1). */
static bool x_tm_null(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETL(0);
}

static bool x_tm_unknown_carrier(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(-1);
}

static const struct rt_method rt_x_telephony[] = {
  M("getImei", "()Ljava/lang/String;", x_tm_null),
  M("getImei", "(I)Ljava/lang/String;", x_tm_null),
  M("getDeviceId", "()Ljava/lang/String;", x_tm_null),
  M("getMeid", "()Ljava/lang/String;", x_tm_null),
  M("getSubscriberId", "()Ljava/lang/String;", x_tm_null),
  M("getSimSerialNumber", "()Ljava/lang/String;", x_tm_null),
  M("getGroupIdLevel1", "()Ljava/lang/String;", x_tm_null),
  M("getLine1Number", "()Ljava/lang/String;", x_tm_null),
  M("getSimCarrierId", "()I", x_tm_unknown_carrier),
  M("getSimSpecificCarrierId", "()I", x_tm_unknown_carrier),
  M_END,
};

/* SubscriptionManager.addOnSubscriptionsChangedListener(): there is nothing that
 * changes, so the listener is accepted and never called. */
static const struct rt_method rt_x_subscription_more[] = {
  M("addOnSubscriptionsChangedListener",
  "(Landroid/telephony/SubscriptionManager$OnSubscriptionsChangedListener;)V", nop_void),
  M("removeOnSubscriptionsChangedListener",
  "(Landroid/telephony/SubscriptionManager$OnSubscriptionsChangedListener;)V", nop_void),
  M_END,
};
static const struct rt_method rt_subscriptions_listener[] = {
  M("<init>", "()V", empty_void),
  M_END,
};

/* --- network, activity manager ------------------------------------------- */
static bool x_empty_string(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  RETL(dvm_new_string(vm, ""));
}

static const struct rt_method rt_x_network_info_more[] = {
  /* The link's radio subtype by name; a wired or Wi-Fi link has none. */
  M("getSubtypeName", "()Ljava/lang/String;", x_empty_string),
  M_END,
};
static const struct rt_method rt_x_activity_manager_more[] = {
  SM("isRunningInTestHarness", "()Z", ret_false),
  SM("isRunningInUserTestHarness", "()Z", ret_false),
  M_END,
};
static const struct rt_method rt_x_traffic_stats_more[] = {
  SM("clearThreadStatsUid", "()V", nop_void),
  SM("setThreadStatsUid", "(I)V", nop_void),
  SM("setThreadStatsTag", "(I)V", nop_void),
  SM("clearThreadStatsTag", "()V", nop_void),
  M_END,
};

/* --- event log, system properties, device config -------------------------- */
static bool x_event_log_write(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(0);
}

static const struct rt_method rt_event_log[] = {
  SM("writeEvent", "(II)I", x_event_log_write),
  SM("writeEvent", "(IJ)I", x_event_log_write),
  SM("writeEvent", "(ILjava/lang/String;)I", x_event_log_write),
  SM("writeEvent", "(I[Ljava/lang/Object;)I", x_event_log_write),
  M_END,
};

/* SystemProperties.getBoolean(): "1/y/yes/on/true" and "0/n/no/off/false" as
 * Android parses them; any other value (or none) is the default. */
static bool x_sysprop_get_boolean(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const char *key = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
  const char *v = key ? android_system_property(key) : NULL;
  static const char *const yes[] = { "1", "y", "yes", "on", "true" };
  static const char *const no[] = { "0", "n", "no", "off", "false" };
  for (size_t i = 0; v && i < 5; ++i) {
    if (!strcmp(v, yes[i])) RETI(1);
    if (!strcmp(v, no[i])) RETI(0);
  }
  RETI(ARG(1).i != 0);
}
/* SystemProperties.getInt/getLong: the property parsed as a number, else the caller's default. */
static bool x_sysprop_get_number(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const char *key = args[0].l ? dvm_string_utf8(vm, args[0].l) : NULL;
  const char *v = key ? android_system_property(key) : NULL;
  const bool is_long = dvm__builtin_method()->sig[strlen(dvm__builtin_method()->sig) - 1] == 'J';
  char *end = NULL;
  if (v && *v) {
    const long long n = strtoll(v, &end, 0);
    if (end != v && *end == 0) {
      if (is_long) RETJ(n);
      if (n >= INT32_MIN && n <= INT32_MAX) RETI((int32_t)n);
    }
  }
  if (is_long) RETJ(args[1].j);
  RETI(args[1].i);
}
static const struct rt_method rt_x_system_properties_more[] = {
  SM("getBoolean", "(Ljava/lang/String;Z)Z", x_sysprop_get_boolean),
  SM("getInt", "(Ljava/lang/String;I)I", x_sysprop_get_number),
  SM("getLong", "(Ljava/lang/String;J)J", x_sysprop_get_number),
  M_END,
};

/* DeviceConfig holds no server-pushed flags on this device: each read is the
 * default the caller gave. */
static bool x_device_config_boolean(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)nargs;
  RETI(ARG(2).i != 0);
}
static bool x_device_config_int(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)nargs;
  RETI(ARG(2).i);
}
static bool x_device_config_long(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)nargs;
  RETJ(ARG(2).j);
}
static bool x_device_config_string(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)nargs;
  RETL(ARG(2).l);
}
static const struct rt_method rt_device_config[] = {
  SM("getBoolean", "(Ljava/lang/String;Ljava/lang/String;Z)Z", x_device_config_boolean),
  SM("getInt", "(Ljava/lang/String;Ljava/lang/String;I)I", x_device_config_int),
  SM("getLong", "(Ljava/lang/String;Ljava/lang/String;J)J", x_device_config_long),
  SM("getString", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
  x_device_config_string),
  M_END,
};

/* TimeUtils.getTimeZoneDatabaseVersion(): the host tz database's release, from
 * the "# version" line that heads its source file. */
static bool x_time_zone_db_version(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  char version[32] = "unknown";
  FILE *f = fopen("/usr/share/zoneinfo/tzdata.zi", "r");
  char line[128];
  if (f && fgets(line, sizeof line, f) && !strncmp(line, "# version ", 10)) {
    line[strcspn(line, " \t\r\n")] = '\0';           /* stops at the first blank */
    snprintf(version, sizeof version, "%.*s", (int)strcspn(line + 10, " \t\r\n"), line + 10);
  }
  if (f) fclose(f);
  RETL(dvm_new_string(vm, version));
}
static const struct rt_method rt_time_utils[] = {
  SM("getTimeZoneDatabaseVersion", "()Ljava/lang/String;", x_time_zone_db_version),
  M_END,
};

/* --- permissions ---------------------------------------------------------- *
 * AppOpsManager.permissionToOp(): the app-op a runtime permission guards, or
 * null for a permission with no op. */
static bool x_permission_to_op(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  static const struct {
    const char *permission, *op;
  } map[] = {
    { "android.permission.ACCESS_COARSE_LOCATION", "android:coarse_location" },
    { "android.permission.ACCESS_FINE_LOCATION", "android:fine_location" },
    { "android.permission.CAMERA", "android:camera" },
    { "android.permission.RECORD_AUDIO", "android:record_audio" },
    { "android.permission.READ_PHONE_STATE", "android:read_phone_state" },
    { "android.permission.READ_CONTACTS", "android:read_contacts" },
    { "android.permission.WRITE_CONTACTS", "android:write_contacts" },
    { "android.permission.READ_CALENDAR", "android:read_calendar" },
    { "android.permission.WRITE_CALENDAR", "android:write_calendar" },
    { "android.permission.READ_EXTERNAL_STORAGE", "android:read_external_storage" },
    { "android.permission.WRITE_EXTERNAL_STORAGE", "android:write_external_storage" },
    { "android.permission.POST_NOTIFICATIONS", "android:post_notification" },
    { "android.permission.SYSTEM_ALERT_WINDOW", "android:system_alert_window" },
    { "android.permission.WRITE_SETTINGS", "android:write_settings" },
    { "android.permission.GET_ACCOUNTS", "android:get_accounts" },
    { "android.permission.BODY_SENSORS", "android:body_sensors" },
    { "android.permission.SEND_SMS", "android:send_sms" },
    { "android.permission.READ_SMS", "android:read_sms" },
    { "android.permission.CALL_PHONE", "android:call_phone" },
  };
  const char *permission = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
  for (size_t i = 0; permission && i < sizeof map / sizeof *map; ++i)
    if (!strcmp(permission, map[i].permission)) RETL(dvm_new_string(vm, map[i].op));
  RETL(0);
}
static const struct rt_method rt_x_app_ops_more[] = {
  SM("permissionToOp", "(Ljava/lang/String;)Ljava/lang/String;", x_permission_to_op),
  M_END,
};

/* DevicePolicyManager.getKeyguardDisabledFeatures(): no policy disables any. */
static const struct rt_method rt_x_device_policy_more[] = {
  M("getKeyguardDisabledFeatures", "(Landroid/content/ComponentName;)I", ret_zero),
  /* A personal device: no profile or device owner is set. */
  M("getProfileOwner", "()Landroid/content/ComponentName;", x_null_ref),
  M("getDeviceOwner", "()Ljava/lang/String;", x_null_ref),
  M("getDeviceOwnerComponentOnCallingUser", "()Landroid/content/ComponentName;", x_null_ref),
  M("isDeviceOwnerApp", "(Ljava/lang/String;)Z", ret_false),
  M("isProfileOwnerApp", "(Ljava/lang/String;)Z", ret_false),
  M_END,
};

/* Settings.System.getConfiguration(resolver, config): copies the persisted
 * configuration settings (font scale) into config.  Nothing is persisted here,
 * so the configuration is left as it is. */
static const struct rt_method rt_x_settings_system_more[] = {
  SM("getConfiguration", "(Landroid/content/ContentResolver;Landroid/content/res/Configuration;)V", empty_void),
  M_END,
};

/* android.nfc.NfcAdapter: this device has no NFC controller, which getDefaultAdapter() reports as null. */
static const struct rt_method rt_nfc_adapter[] = {
  SM("getDefaultAdapter", "(Landroid/content/Context;)Landroid/nfc/NfcAdapter;", x_null_ref),
  M("isEnabled", "()Z", ret_false),
  M_END,
};

/* SparseArray.removeAtRange(index, size): drops `size` entries starting at `index`;
 * each removal shifts the rest down, so the same index is removed `size` times. */
static bool x_sparse_remove_range(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)nargs;
  (void)out;
  for (int32_t i = 0; i < ARG(1).i; ++i) {
    union dvm_value at = { .i = ARG(0).i }, ignored = { 0 };
    if (!am_vcall(vm, self, "removeAt", "(I)V", &at, 1, &ignored)) return false;
  }
  RETV();
}
static const struct rt_method rt_x_sparse_array_more[] = {
  M("removeAtRange", "(II)V", x_sparse_remove_range),
  M_END,
};

/* --- small classes with a constructor and little else ---------------------- */
static bool x_work_source_add(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(0);
}
static const struct rt_method rt_work_source[] = {
  M("<init>", "()V", empty_void),
  M("add", "(I)Z", x_work_source_add),
  M("add", "(ILjava/lang/String;)Z", x_work_source_add),
  M("size", "()I", ret_zero),
  M("isEmpty", "()Z", ret_true),
  M("clear", "()V", nop_void),
  M_END,
};
static const struct rt_method rt_backup_manager[] = {
  M("<init>", "(Landroid/content/Context;)V", empty_void),
  M("dataChanged", "()V", nop_void),
  SM("dataChanged", "(Ljava/lang/String;)V", nop_void),
  M_END,
};
static const struct rt_method rt_x_package_info_more[] = {
  M("<init>", "()V", empty_void),
  M_END,
};

/* --- PendingIntent: who created it -------------------------------------- */
static bool x_pending_creator_package(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                      int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  dvm_ref token = pending_field(vm, self, "token", "Landroid/app/PendingIntent;");
  RETL(token ? pending_field(vm, token, "creator", "Ljava/lang/String;") : 0);
}
static bool x_pending_creator_uid(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI((int32_t)arm_exec_guest_uid());
}
static const struct rt_method rt_x_pending_intent_more[] = {
  M("getCreatorPackage", "()Ljava/lang/String;", x_pending_creator_package),
  M("getTargetPackage", "()Ljava/lang/String;", x_pending_creator_package),
  M("getCreatorUid", "()I", x_pending_creator_uid),
  M_END,
};

/* --- DatabaseUtils: one value from a query ------------------------------- *
 * rawQuery(sql, args), then the first column of the first row; no row is an
 * SQLiteDoneException, which the framework's own helpers throw the same way. */
static bool x_db_first_cell(struct dvm *vm, const union dvm_value *args, const char *getter,
                            const char *getter_sig, union dvm_value *value)
{
  union dvm_value query[2] = { args[1], args[2] }, cursor = { 0 }, moved = { 0 };
  if (!am_vcall(vm, args[0].l, "rawQuery", "(Ljava/lang/String;[Ljava/lang/String;)Landroid/database/Cursor;",
                query, 2, &cursor) || !cursor.l) return false;
  dvm_pin(vm, cursor.l);
  bool ok = am_vcall(vm, cursor.l, "moveToFirst", "()Z", NULL, 0, &moved);
  if (ok && !moved.i) {
    dvm__throw(vm, "android/database/sqlite/SQLiteDoneException",
               "Don't have database data to return");
    ok = false;
  } else if (ok) {
    union dvm_value column = { .i = 0 };
    ok = am_vcall(vm, cursor.l, getter, getter_sig, &column, 1, value);
  }
  union dvm_value ignored = { 0 };
  (void)am_vcall(vm, cursor.l, "close", "()V", NULL, 0, &ignored);
  dvm_unpin(vm, cursor.l);
  return ok;
}
static bool x_db_string_for_query(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)self;
  union dvm_value in[3] = { ARG(0), ARG(1), nargs > 2 ? ARG(2) : (union dvm_value) {
    0
  }
                          };
  union dvm_value v = { 0 };
  if (!x_db_first_cell(vm, in, "getString", "(I)Ljava/lang/String;", &v)) return false;
  RETL(v.l);
}
static bool x_db_long_for_query(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  (void)self;
  union dvm_value in[3] = { ARG(0), ARG(1), nargs > 2 ? ARG(2) : (union dvm_value) {
    0
  }
                          };
  union dvm_value v = { 0 };
  if (!x_db_first_cell(vm, in, "getLong", "(I)J", &v)) return false;
  RETJ(v.j);
}
/* DatabaseUtils.queryNumEntries(db, table[, selection[, selectionArgs]]): select count(*). */
static bool x_db_num_entries(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
  (void)self;
  const char *table = args[1].l ? dvm_string_utf8(vm, args[1].l) : NULL;
  const char *where = nargs > 2 && args[2].l ? dvm_string_utf8(vm, args[2].l) : NULL;
  if (!table) {
    dvm__throw(vm, "java/lang/NullPointerException", "table");
    return false;
  }
  const size_t n = strlen(table) + (where ? strlen(where) : 0) + 40;
  char *sql = malloc(n);
  if (!sql) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "queryNumEntries");
    return false;
  }
  snprintf(sql, n, "select count(*) from %s%s%s", table, where && *where ? " where " : "", where ? where : "");
  union dvm_value in[3] = { args[0], { .l = dvm_new_string(vm, sql) }, { .l = nargs > 3 ? args[3].l : 0 } };
  free(sql);
  union dvm_value v = { 0 };
  if (!x_db_first_cell(vm, in, "getLong", "(I)J", &v)) return false;
  RETJ(v.j);
}
/* Android API 31 DatabaseUtils classifies by the first three UTF-16 units,
 * after String.trim(), rather than parsing SQL or skipping comments. */
static unsigned x_db_keyword_upper(unsigned c)
{
  if(c>='a' && c<='z') return c-'a'+'A';
  /* The non-ASCII single-unit ROOT uppercase mappings into ASCII. Expanding
   * mappings cannot turn a three-unit prefix into a three-letter keyword. */
  if(c==0x131) return 'I';
  if(c==0x17f) return 'S';
  return c;
}
static bool x_db_statement_type(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                int nargs,union dvm_value *out)
{
  (void)self;
  (void)nargs;
  if(!ARG(0).l) {
    dvm__throw(vm,"java/lang/NullPointerException","sql");
    return false;
  }
  struct ustr sql;
  if(ustr_of(vm,ARG(0).l,&sql)<=0) return false;
  uint32_t begin=0,end=sql.length;
  while(begin<end && ustr_at(&sql,begin)<=32) ++begin;
  while(end>begin && ustr_at(&sql,end-1)<=32) --end;
  if(end-begin<3) RETI(99);
  char prefix[4]={0};
  for(unsigned i=0;i<3;++i) {
    unsigned c=x_db_keyword_upper(ustr_at(&sql,begin+i));
    if(c>127) RETI(99);
    prefix[i]=(char)c;
  }
  static const struct {const char *prefix; int type;} types[]={
    {"SEL",1},{"INS",2},{"UPD",2},{"REP",2},{"DEL",2},{"ATT",3},
    {"BEG",4},{"COM",5},{"END",5},{"PRA",7},{"CRE",8},{"DRO",8},
    {"ALT",8},{"ANA",9},{"DET",9}
  };
  if(!strcmp(prefix,"ROL")) {
    for(uint32_t i=begin;i+3<end;++i)
      if(ustr_at(&sql,i)==' ' && x_db_keyword_upper(ustr_at(&sql,i+1))=='T' &&
         x_db_keyword_upper(ustr_at(&sql,i+2))=='O' && ustr_at(&sql,i+3)==' ') RETI(99);
    RETI(6);
  }
  for(size_t i=0;i<sizeof types/sizeof types[0];++i)
    if(!strcmp(prefix,types[i].prefix)) RETI(types[i].type);
  RETI(99);
}
static const struct rt_method rt_database_utils[] = {
  SM("getSqlStatementType", "(Ljava/lang/String;)I", x_db_statement_type),
  SM("stringForQuery", "(Landroid/database/sqlite/SQLiteDatabase;Ljava/lang/String;[Ljava/lang/String;)Ljava/lang/String;",
  x_db_string_for_query),
  SM("longForQuery", "(Landroid/database/sqlite/SQLiteDatabase;Ljava/lang/String;[Ljava/lang/String;)J",
  x_db_long_for_query),
  SM("queryNumEntries", "(Landroid/database/sqlite/SQLiteDatabase;Ljava/lang/String;)J", x_db_num_entries),
  SM("queryNumEntries", "(Landroid/database/sqlite/SQLiteDatabase;Ljava/lang/String;Ljava/lang/String;)J", x_db_num_entries),
  SM("queryNumEntries", "(Landroid/database/sqlite/SQLiteDatabase;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)J", x_db_num_entries),
  M_END,
};

/* The remaining hidden queries a system app probes for by reflection. */
static const struct rt_method rt_x_power_manager_more[] = {
  M("isLightDeviceIdleMode", "()Z", ret_false),
  M("isDeviceLightIdleMode", "()Z", ret_false),
  M_END,
};
static const struct rt_method rt_x_usage_stats_more[] = {
  M("whitelistAppTemporarily", "(Ljava/lang/String;JLandroid/os/UserHandle;)V", nop_void),
  M("isAppInactive", "(Ljava/lang/String;)Z", ret_false),
  M("getAppStandbyBucket", "()I", ret_zero),
  M_END,
};
static bool x_conn_is_mobile_type(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)nargs;
  /* TYPE_MOBILE and the carrier-specific mobile types (MMS, SUPL, DUN, HIPRI). */
  const int t = ARG(0).i;
  RETI(t == 0 || t == 2 || t == 3 || t == 4 || t == 5 || t == 10 || t == 11 ||
       t == 12 || t == 14 || t == 15);
}
static const struct rt_method rt_x_conn_more[] = {
  SM("isNetworkTypeMobile", "(I)Z", x_conn_is_mobile_type),
  M_END,
};

/* --- dalvik.system.BlockGuard ------------------------------------------- *
 * StrictMode's hook: libcore calls the thread's Policy before blocking on the
 * network or the disk.  The default is LAX_POLICY, which permits everything; an
 * app (or StrictMode) may install its own.  The policy is per thread. */
static __thread dvm_ref g_block_guard_policy;   /* 0: the thread has the default */
static dvm_ref g_block_guard_lax;

static dvm_ref block_guard_lax(struct dvm *vm)
{
  return runtime_singleton(vm, "Ldalvik/system/BlockGuard$1;", &g_block_guard_lax);
}

static bool block_guard_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  RETL(g_block_guard_policy ? g_block_guard_policy : block_guard_lax(vm));
}

static bool block_guard_set(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  (void)out;
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "policy == null");
    return false;
  }
  if (g_block_guard_policy) dvm_unpin(vm, g_block_guard_policy);
  g_block_guard_policy = ARG(0).l;
  dvm_pin(vm, g_block_guard_policy);
  RETV();
}

static bool x_block_guard_name(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  RETL(dvm_new_string(vm, "LAX_POLICY"));
}

static bool block_guard_policy_threshold(struct dvm *vm, dvm_ref self, const union dvm_value *args,
    int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(0);
}

static const struct rt_method rt_block_guard[] = {
  SM("getThreadPolicy", "()Ldalvik/system/BlockGuard$Policy;", block_guard_get),
  SM("setThreadPolicy", "(Ldalvik/system/BlockGuard$Policy;)V", block_guard_set),
  M_END,
};
/* Policy is an interface; LAX_POLICY (BlockGuard$1) permits every operation. */
static const struct rt_method rt_block_guard_policy[] = {
  {"onWriteToDisk", "()V", NULL, DEX_ACC_ABSTRACT},
  {"onReadFromDisk", "()V", NULL, DEX_ACC_ABSTRACT},
  {"onNetwork", "()V", NULL, DEX_ACC_ABSTRACT},
  {"onUnbufferedIO", "()V", NULL, DEX_ACC_ABSTRACT},
  {"onExplicitGc", "()V", NULL, DEX_ACC_ABSTRACT},
  {"getPolicyMask", "()I", NULL, DEX_ACC_ABSTRACT},
  M_END,
};
static const struct rt_method rt_block_guard_lax[] = {
  M("onWriteToDisk", "()V", nop_void),
  M("onReadFromDisk", "()V", nop_void),
  M("onNetwork", "()V", nop_void),
  M("onUnbufferedIO", "()V", nop_void),
  M("onExplicitGc", "()V", nop_void),
  M("getPolicyMask", "()I", block_guard_policy_threshold),
  M("toString", "()Ljava/lang/String;", x_block_guard_name),
  M_END,
};
static const char *const rt_iface_block_guard_policy[] = { "Ldalvik/system/BlockGuard$Policy;", NULL };

/* --- javax.net.ssl.SSLEngine ------------------------------------------- *
 * The abstract base of every TLS engine.  What it carries itself is small -- the
 * peer's host and port, and the convenience overloads of wrap()/unwrap() that
 * hand the one-buffer forms to the array forms the engine implements. */
static bool ssl_engine_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)out;
  if (nargs >= 2) {
    (void)dvm_set_field(vm, self, "peerHost", "Ljava/lang/String;", ARG(0));
    (void)dvm_set_field(vm, self, "peerPort", "I", ARG(1));
  } else {
    (void)dvm_set_field(vm, self, "peerPort", "I", (union dvm_value) {
      .i = -1
    });
  }
  RETV();
}

static bool ssl_engine_peer(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  if (!strcmp(builtin_name(), "getPeerHost"))
    return dvm_get_field(vm, self, "peerHost", "Ljava/lang/String;", out);
  return dvm_get_field(vm, self, "peerPort", "I", out);
}

/* A one-element ByteBuffer[] holding `buffer`. */
static dvm_ref ssl_engine_one(struct dvm *vm, dvm_ref buffer)
{
  dvm_ref array = dvm_new_array(vm, 'L', "Ljava/nio/ByteBuffer;", 1);
  if (array) ((dvm_ref *)dvm_array_data(vm, array))[0] = buffer;
  return array;
}

/* wrap(src, dst) / wrap(srcs, dst) -> wrap(srcs, 0, srcs.length, dst). */
static bool ssl_engine_wrap(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)nargs;
  dvm_ref sources = ARG(0).l;
  const char *sig = builtin_sig();
  if (!strncmp(sig, "(Ljava/nio/ByteBuffer;", 22)) sources = ssl_engine_one(vm, ARG(0).l);
  if (!sources) return false;
  dvm_pin(vm, sources);
  struct dvm_object *array = dvm__obj(vm, sources);
  union dvm_value call[4] = { { .l = sources }, { .i = 0 },
    { .i = array ? (int32_t)array->length : 0 }, ARG(1)
  };
  const bool ok = am_vcall(vm, self, "wrap",
                           "([Ljava/nio/ByteBuffer;IILjava/nio/ByteBuffer;)Ljavax/net/ssl/SSLEngineResult;", call, 4, out);
  dvm_unpin(vm, sources);
  return ok;
}

/* unwrap(src, dst) / unwrap(src, dsts) -> unwrap(src, dsts, 0, dsts.length). */
static bool ssl_engine_unwrap(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
  (void)nargs;
  dvm_ref sinks = ARG(1).l;
  const char *sig = builtin_sig();
  if (!strncmp(sig, "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;)", 46)) sinks = ssl_engine_one(vm, ARG(1).l);
  if (!sinks) return false;
  dvm_pin(vm, sinks);
  struct dvm_object *array = dvm__obj(vm, sinks);
  union dvm_value call[4] = { ARG(0), { .l = sinks }, { .i = 0 },
    { .i = array ? (int32_t)array->length : 0 }
  };
  const bool ok = am_vcall(vm, self, "unwrap",
                           "(Ljava/nio/ByteBuffer;[Ljava/nio/ByteBuffer;II)Ljavax/net/ssl/SSLEngineResult;", call, 4, out);
  dvm_unpin(vm, sinks);
  return ok;
}

static bool ssl_engine_null(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETL(0);
}


/* --- lunaria.HostSSLEngine: the built-in SSLContext's engine ---------------
 * A real TLS session over memory BIOs in the host's TLS library.  The
 * caller moves the bytes (wrap() produces what goes to the peer, unwrap() takes
 * what came from it), exactly as with Conscrypt's engine.  Verification is the
 * platform's: the chain against the host trust store, and the name when the
 * endpoint identification algorithm is set. */
struct host_engine {
  struct dvm_tls_engine *tls;
  bool client, began, reported, in_done, out_done, verify_host, session_creation;
  bool need_client_auth, want_client_auth;
  char **protocols, **ciphers, **alpn;
  int nprotocols, nciphers, nalpn;
  char peer_host[256];
  int peer_port;
  char error[256];
};

static struct host_engine *host_engine_of(struct dvm *vm, dvm_ref self)
{
  struct dvm_object *o = dvm__obj(vm, self);
  if (!o) return NULL;
  if (!o->data) {
    struct host_engine *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->client = true;
    h->session_creation = true;
    h->peer_port = -1;
    o->data = h;
  }
  return o->data;
}

static void host_engine_set_list(char ***dst, int *n, char **v, int nv)
{
  string_list_free(*dst, *n);
  *dst = v;
  *n = nv;
}

/* The TLS session, made on first use from what the engine was configured with
 * (setEnabledProtocols() and friends are legal until the handshake begins). */
static bool host_engine_tls(struct dvm *vm, struct host_engine *h)
{
  (void)vm;
  if (h->tls) return true;
  int minv = 0, maxv = 0;
  for (int i = 0; i < h->nprotocols; ++i) {
    const int v = tls_version_of(h->protocols[i]);
    if (!v) continue;
    if (!minv || v < minv) minv = v;
    if (!maxv || v > maxv) maxv = v;
  }
  if (!h->nprotocols) {
    minv = TLS1_2_VERSION;
    maxv = TLS1_3_VERSION;
  }
  uint8_t wire[512];
  size_t wlen = 0;
  for (int i = 0; i < h->nalpn; ++i) {
    const size_t l = strlen(h->alpn[i]);
    if (!l || l > 255 || wlen + 1 + l > sizeof wire) continue;
    wire[wlen++] = (uint8_t)l;
    memcpy(wire + wlen, h->alpn[i], l);
    wlen += l;
  }
  h->tls = dvm_tls_engine_new(h->client, h->peer_host,
                              DVM_TLS_VERIFY_CHAIN | (h->verify_host ? DVM_TLS_VERIFY_HOST : 0),
                              wlen ? wire : NULL, wlen, minv, maxv,
                              (const char *const *)h->ciphers, h->nciphers);
  return h->tls != NULL;
}

static bool host_engine_fail(struct dvm *vm, struct host_engine *h, const char *why)
{
  snprintf(h->error, sizeof h->error, "%s", why);
  dvm__throw(vm, "javax/net/ssl/SSLHandshakeException", "%s", why);
  return false;
}

/* A one-shot SSLEngineResult. */
static bool host_engine_result(struct dvm *vm, const char *status, const char *hs,
                               int consumed, int produced, union dvm_value *out)
{
  struct dvm_class *c = dvm__class_by_desc(vm, "Ljavax/net/ssl/SSLEngineResult;");
  dvm_ref r = c ? dvm_new_object(vm, c) : 0;
  if (!r) return false;
  dvm_pin(vm, r);
  union dvm_value a[4] = { 0 }, ign;
  struct dvm_class *sc = dvm__class_by_desc(vm, "Ljavax/net/ssl/SSLEngineResult$Status;");
  struct dvm_class *hc = dvm__class_by_desc(vm, "Ljavax/net/ssl/SSLEngineResult$HandshakeStatus;");
  if (!sc || !hc || !dvm_get_static(vm, sc, status, "Ljavax/net/ssl/SSLEngineResult$Status;", &a[0]) ||
      !dvm_get_static(vm, hc, hs, "Ljavax/net/ssl/SSLEngineResult$HandshakeStatus;", &a[1])) {
    dvm_unpin(vm, r);
    return false;
  }
  a[2].i = consumed;
  a[3].i = produced;
  const bool ok = am_vcall(vm, r, "<init>",
                           "(Ljavax/net/ssl/SSLEngineResult$Status;Ljavax/net/ssl/SSLEngineResult$HandshakeStatus;II)V",
                           a, 4, &ign);
  dvm_unpin(vm, r);
  if (ok) out->l = r;
  return ok;
}

/* The handshake status the engine is in now; FINISHED is reported once, by the
 * call that completed the handshake and left nothing of it unsent. */
static const char *host_engine_hs(struct host_engine *h, bool consume_finished)
{
  if (!h->tls || !h->began) return "NOT_HANDSHAKING";
  const bool established = dvm_tls_engine_established(h->tls);
  const bool pending = dvm_tls_engine_pending_out(h->tls) > 0;
  if (established && !h->reported) {
    if (pending) return "NEED_WRAP";
    if (consume_finished) h->reported = true;
    return "FINISHED";
  }
  if (established) return "NOT_HANDSHAKING";
  if (pending) return "NEED_WRAP";
  return "NEED_UNWRAP";
}

static bool host_engine_begin(struct dvm *vm, struct host_engine *h)
{
  if (!host_engine_tls(vm, h)) return host_engine_fail(vm, h, "SSL_new failed");
  if (h->began) return true;
  h->began = true;
  char err[256] = "";
  if (dvm_tls_engine_handshake(h->tls, err, sizeof err) < 0) return host_engine_fail(vm, h, err);
  return true;
}

static bool x_hse_begin(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
  (void)args;
  (void)nargs;
  (void)out;
  struct host_engine *h = host_engine_of(vm, self);
  if (!h) return false;
  if (!host_engine_begin(vm, h)) return false;
  RETV();
}

static bool x_hse_hs_status(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)args;
  (void)nargs;
  struct host_engine *h = host_engine_of(vm, self);
  struct dvm_class *hc = dvm__class_by_desc(vm, "Ljavax/net/ssl/SSLEngineResult$HandshakeStatus;");
  if (!h || !hc) return false;
  /* A finished handshake reads as NOT_HANDSHAKING here, as in the JDK. */
  const char *st = host_engine_hs(h, false);
  if (!strcmp(st, "FINISHED")) st = "NOT_HANDSHAKING";
  return dvm_get_static(vm, hc, st, "Ljavax/net/ssl/SSLEngineResult$HandshakeStatus;", out);
}

/* Remaining bytes of a ByteBuffer, and moving bytes in and out of one. */
static int hse_remaining(struct dvm *vm, dvm_ref bb)
{
  union dvm_value v = { 0 };
  return am_vcall(vm, bb, "remaining", "()I", NULL, 0, &v) ? v.i : 0;
}

static bool hse_get(struct dvm *vm, dvm_ref bb, uint8_t *dst, int n)
{
  dvm_ref arr = dvm_new_array(vm, 'B', "B", (uint32_t)n);
  if (!arr) return false;
  dvm_pin(vm, arr);
  union dvm_value a[3] = { { .l = arr }, { .i = 0 }, { .i = n } }, ign;
  const bool ok = am_vcall(vm, bb, "get", "([BII)Ljava/nio/ByteBuffer;", a, 3, &ign);
  if (ok) memcpy(dst, dvm_array_data(vm, arr), (size_t)n);
  dvm_unpin(vm, arr);
  return ok;
}

static bool hse_put(struct dvm *vm, dvm_ref bb, const uint8_t *src, int n)
{
  dvm_ref arr = dvm_new_array(vm, 'B', "B", (uint32_t)n);
  if (!arr) return false;
  dvm_pin(vm, arr);
  memcpy(dvm_array_data(vm, arr), src, (size_t)n);
  union dvm_value a[3] = { { .l = arr }, { .i = 0 }, { .i = n } }, ign;
  const bool ok = am_vcall(vm, bb, "put", "([BII)Ljava/nio/ByteBuffer;", a, 3, &ign);
  dvm_unpin(vm, arr);
  return ok;
}

#define HSE_RECORD 16709   /* the largest TLS record: payload + header + overhead */

/* wrap(ByteBuffer[] srcs, int off, int len, ByteBuffer dst) */
static bool x_hse_wrap(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  (void)nargs;
  struct host_engine *h = host_engine_of(vm, self);
  if (!h) return false;
  if (!host_engine_begin(vm, h)) return false;
  dvm_ref dst = ARG(3).l;
  struct dvm_object *srcs = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
  const int off = ARG(1).i, cnt = ARG(2).i;
  struct dvm_tls_engine *t = h->tls;
  uint8_t *buf = malloc(HSE_RECORD);
  if (!buf) return false;
  const int room = hse_remaining(vm, dst);
  int consumed = 0, produced = 0;
  char err[256] = "";
  bool ok = true;

  if (h->out_done && !dvm_tls_engine_pending_out(t)) {
    free(buf);
    return host_engine_result(vm, "CLOSED", "NOT_HANDSHAKING", 0, 0, out);
  }
  if (!dvm_tls_engine_established(t)) {
    int r = dvm_tls_engine_handshake(t, err, sizeof err);
    if (r < 0) {
      free(buf);
      return host_engine_fail(vm, h, err);
    }
  } else if (!h->out_done && srcs && srcs->data) {
    /* Application data: one record's worth of the sources, if there is room for
     * the record (the caller sizes dst by getSession().getPacketBufferSize()). */
    const dvm_ref *items = srcs->data;
    for (int i = off; i < off + cnt && ok && room >= HSE_RECORD; ++i) {
      const int rem = items[i] ? hse_remaining(vm, items[i]) : 0;
      if (rem <= 0) continue;
      const int n = rem > 16384 ? 16384 : rem;
      uint8_t *plain = malloc((size_t)n);
      if (!plain) {
        ok = false;
        break;
      }
      ok = hse_get(vm, items[i], plain, n);
      if (ok) {
        long w = dvm_tls_engine_write(t, plain, (size_t)n, err, sizeof err);
        if (w < 0) ok = false;
        else consumed += (int)w;
      }
      free(plain);
      break;
    }
    if (!ok) {
      free(buf);
      if (!dvm_exception(vm)) host_engine_fail(vm, h, err[0] ? err : "SSL write failed");
      return false;
    }
  }
  if (h->out_done) dvm_tls_engine_shutdown(t);
  if (room > 0) {
    const size_t cap = (size_t)room < HSE_RECORD ? (size_t)room : HSE_RECORD;
    produced = (int)dvm_tls_engine_drain(t, buf, cap);
    if (produced && !hse_put(vm, dst, buf, produced)) ok = false;
  }
  const bool overflow = room <= 0 && dvm_tls_engine_pending_out(t) > 0;
  free(buf);
  if (!ok) return false;
  const char *status = overflow ? "BUFFER_OVERFLOW"
                       : (h->out_done && !dvm_tls_engine_pending_out(t)) ? "CLOSED" : "OK";
  return host_engine_result(vm, status, host_engine_hs(h, true), consumed, produced, out);
}

/* unwrap(ByteBuffer src, ByteBuffer[] dsts, int off, int len) */
static bool x_hse_unwrap(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)nargs;
  struct host_engine *h = host_engine_of(vm, self);
  if (!h) return false;
  if (!host_engine_begin(vm, h)) return false;
  struct dvm_tls_engine *t = h->tls;
  dvm_ref src = ARG(0).l;
  struct dvm_object *dsts = ARG(1).l ? dvm__obj(vm, ARG(1).l) : NULL;
  const int off = ARG(2).i, cnt = ARG(3).i;
  char err[256] = "";
  if (h->in_done) return host_engine_result(vm, "CLOSED", host_engine_hs(h, false), 0, 0, out);

  int room = 0;
  const dvm_ref *items = dsts ? dsts->data : NULL;
  for (int i = off; items && i < off + cnt; ++i)
    if (items[i]) room += hse_remaining(vm, items[i]);
  const int avail = src ? hse_remaining(vm, src) : 0;
  const bool buffered = dvm_tls_engine_pending_in(t) > 0;
  if (!avail && !buffered)
    return host_engine_result(vm, "BUFFER_UNDERFLOW", host_engine_hs(h, false), 0, 0, out);
  /* Decrypted bytes the session already holds must find room before more is fed. */
  if (buffered && room <= 0)
    return host_engine_result(vm, "BUFFER_OVERFLOW", host_engine_hs(h, false), 0, 0, out);

  int consumed = 0, produced = 0;
  if (avail) {
    uint8_t *net = malloc((size_t)avail);
    if (!net || !hse_get(vm, src, net, avail)) {
      free(net);
      return false;
    }
    consumed = (int)dvm_tls_engine_feed(t, net, (size_t)avail);
    free(net);
  }
  if (!dvm_tls_engine_established(t)) {
    if (dvm_tls_engine_handshake(t, err, sizeof err) < 0) return host_engine_fail(vm, h, err);
  }
  bool closed = false;
  if (dvm_tls_engine_established(t) && room > 0) {
    uint8_t *plain = malloc(16384);
    if (!plain) return false;
    for (int i = off; items && i < off + cnt && !closed; ++i) {
      int space = items[i] ? hse_remaining(vm, items[i]) : 0;
      while (space > 0) {
        const long r = dvm_tls_engine_read(t, plain, (size_t)(space < 16384 ? space : 16384), err, sizeof err);
        if (r == -1) {
          closed = true;
          break;
        }
        if (r == -2) {
          free(plain);
          return host_engine_fail(vm, h, err);
        }
        if (r == 0) break;
        if (!hse_put(vm, items[i], plain, (int)r)) {
          free(plain);
          return false;
        }
        produced += (int)r;
        space -= (int)r;
      }
      if (space > 0) break;   /* the session has no more for now */
    }
    free(plain);
  }
  if (closed || dvm_tls_engine_peer_closed(t)) {
    h->in_done = true;
    /* close_notify is answered by closing our side too. */
    if (!h->out_done) {
      h->out_done = true;
      dvm_tls_engine_shutdown(t);
    }
  }
  return host_engine_result(vm, h->in_done ? "CLOSED" : "OK", host_engine_hs(h, true), consumed, produced, out);
}

static bool x_hse_close(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
  (void)args;
  (void)nargs;
  (void)out;
  struct host_engine *h = host_engine_of(vm, self);
  if (!h) return false;
  if (!strcmp(builtin_name(), "closeInbound")) {
    /* Without the peer's close_notify the data may have been truncated. */
    if (!h->in_done && h->tls && dvm_tls_engine_established(h->tls) && !dvm_tls_engine_peer_closed(h->tls)) {
      h->in_done = true;
      dvm__throw(vm, "javax/net/ssl/SSLException",
                 "Inbound closed before receiving peer's close_notify: possible truncation attack?");
      return false;
    }
    h->in_done = true;
  } else {
    h->out_done = true;
    if (h->tls) dvm_tls_engine_shutdown(h->tls);
  }
  RETV();
}

static bool x_hse_done(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  (void)args;
  (void)nargs;
  struct host_engine *h = host_engine_of(vm, self);
  if (!h) return false;
  if (!strcmp(builtin_name(), "isInboundDone")) RETI(h->in_done);
  RETI(h->out_done && (!h->tls || !dvm_tls_engine_pending_out(h->tls)));
}

static bool x_hse_task(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  /* The host library finishes every step inside wrap()/unwrap(): no delegated tasks. */
  RETL(0);
}

static bool x_hse_supported(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  if (!strcmp(builtin_name(), "getSupportedProtocols")) RETL(string_array_of(vm, g_ssl_protocols, 4));
  const char *const *all = dvm_tls_supported_ciphers();
  RETL(string_array_of(vm, all, cipher_count(all)));
}

static bool x_hse_enabled(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  (void)args;
  (void)nargs;
  struct host_engine *h = host_engine_of(vm, self);
  if (!h) return false;
  if (!strcmp(builtin_name(), "getEnabledProtocols")) {
    if (h->nprotocols) RETL(string_array_of(vm, (const char *const *)h->protocols, h->nprotocols));
    RETL(string_array_of(vm, g_ssl_default_protocols, 2));
  }
  if (h->nciphers) RETL(string_array_of(vm, (const char *const *)h->ciphers, h->nciphers));
  const char *const *all = dvm_tls_supported_ciphers();
  RETL(string_array_of(vm, all, cipher_count(all)));
}

static bool x_hse_set_enabled(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
  (void)nargs;
  (void)out;
  struct host_engine *h = host_engine_of(vm, self);
  if (!h) return false;
  const bool protocols = !strcmp(builtin_name(), "setEnabledProtocols");
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", protocols ? "protocols == null" : "suites == null");
    return false;
  }
  char **v = NULL;
  const int n = string_array_take(vm, ARG(0).l, &v);
  for (int i = 0; i < n; ++i) {
    bool ok = false;
    if (protocols) {
      for (int k = 0; k < 4 && !ok; ++k) ok = !strcmp(v[i], g_ssl_protocols[k]);
    } else {
      const char *const *all = dvm_tls_supported_ciphers();
      for (int k = 0; all[k] && !ok; ++k) ok = !strcmp(v[i], all[k]);
      ok |= !strcmp(v[i], "TLS_EMPTY_RENEGOTIATION_INFO_SCSV") || !strcmp(v[i], "TLS_FALLBACK_SCSV");
    }
    if (!ok) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 protocols ? "Protocol %s is not supported" : "Unsupported cipher suite: %s", v[i]);
      string_list_free(v, n);
      return false;
    }
  }
  if (protocols) host_engine_set_list(&h->protocols, &h->nprotocols, v, n);
  else host_engine_set_list(&h->ciphers, &h->nciphers, v, n);
  RETV();
}

static bool x_hse_mode(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  (void)nargs;
  struct host_engine *h = host_engine_of(vm, self);
  if (!h) return false;
  const char *name = builtin_name();
  if (!strcmp(name, "setUseClientMode")) {
    if (h->tls) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Cannot change mode after handshake has started");
      return false;
    }
    h->client = ARG(0).i != 0;
    RETV();
  }
  if (!strcmp(name, "getUseClientMode")) RETI(h->client);
  if (!strcmp(name, "setNeedClientAuth")) {
    h->need_client_auth = ARG(0).i != 0;
    if (h->need_client_auth) h->want_client_auth = false;
    RETV();
  }
  if (!strcmp(name, "getNeedClientAuth")) RETI(h->need_client_auth);
  if (!strcmp(name, "setWantClientAuth")) {
    h->want_client_auth = ARG(0).i != 0;
    if (h->want_client_auth) h->need_client_auth = false;
    RETV();
  }
  if (!strcmp(name, "getWantClientAuth")) RETI(h->want_client_auth);
  if (!strcmp(name, "setEnableSessionCreation")) {
    h->session_creation = ARG(0).i != 0;
    RETV();
  }
  RETI(h->session_creation);
}

static bool x_hse_alpn(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  (void)args;
  (void)nargs;
  struct host_engine *h = host_engine_of(vm, self);
  if (!h) return false;
  /* null until the handshake is done, "" when no protocol was agreed. */
  if (!h->tls || !dvm_tls_engine_established(h->tls)) RETL(0);
  char sel[64];
  dvm_tls_engine_alpn(h->tls, sel, sizeof sel);
  RETL(dvm_new_string(vm, sel));
}

static bool x_hse_session(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  (void)args;
  (void)nargs;
  struct host_engine *h = host_engine_of(vm, self);
  struct dvm_class *c = dvm__class_by_desc(vm, "Lcom/android/org/conscrypt/ActiveSession;");
  dvm_ref sess = c ? dvm_new_object(vm, c) : 0;
  if (!h || !sess) RETL(sess);
  dvm_pin(vm, sess);
  const bool up = h->tls && dvm_tls_engine_established(h->tls);
  union dvm_value v = { 0 };
  /* Before the handshake: the null session (SSL_NULL_WITH_NULL_NULL / NONE). */
  v.l = dvm_new_string(vm, up ? dvm_tls_engine_cipher(h->tls) : "SSL_NULL_WITH_NULL_NULL");
  (void)dvm_set_field(vm, sess, "cipherSuite", "Ljava/lang/String;", v);
  v.l = dvm_new_string(vm, up ? dvm_tls_engine_protocol(h->tls) : "NONE");
  (void)dvm_set_field(vm, sess, "protocol", "Ljava/lang/String;", v);
  v.l = dvm_new_string(vm, h->peer_host);
  (void)dvm_set_field(vm, sess, "peerHost", "Ljava/lang/String;", v);
  v.i = h->peer_port;
  (void)dvm_set_field(vm, sess, "peerPort", "I", v);
  v.j = (int64_t)time(NULL) * 1000;
  (void)dvm_set_field(vm, sess, "creationTime", "J", v);
  v.i = up;
  (void)dvm_set_field(vm, sess, "valid", "Z", v);
  if (up) {
    uint8_t **der = NULL;
    int *len = NULL;
    const int n = dvm_tls_engine_peer_chain(h->tls, &der, &len);
    dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/security/cert/Certificate;", (uint32_t)n);
    if (arr) dvm_pin(vm, arr);
    for (int i = 0; i < n; ++i) {
      dvm_ref cert = arr ? x509_from_der(vm, der[i], len[i]) : 0;
      dvm_ref *items = arr ? dvm_array_data(vm, arr) : NULL;
      if (items) items[i] = cert;
      free(der[i]);
    }
    free(der);
    free(len);
    v.l = arr;
    (void)dvm_set_field(vm, sess, "peerCertificates", "[Ljava/security/cert/Certificate;", v);
    if (arr) dvm_unpin(vm, arr);
  }
  dvm_unpin(vm, sess);
  RETL(sess);
}

static bool x_hse_get_params(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  (void)args;
  (void)nargs;
  struct host_engine *h = host_engine_of(vm, self);
  struct dvm_class *c = dvm__class_by_desc(vm, "Ljavax/net/ssl/SSLParameters;");
  dvm_ref p = c ? dvm_new_object(vm, c) : 0;
  if (!p || !h) RETL(p);
  dvm_pin(vm, p);
  union dvm_value en = { 0 }, v = { 0 };
  const char *const *all = dvm_tls_supported_ciphers();
  v.l = h->nciphers ? string_array_of(vm, (const char *const *)h->ciphers, h->nciphers)
        : string_array_of(vm, all, cipher_count(all));
  (void)dvm_set_field(vm, p, "cipherSuites", "[Ljava/lang/String;", v);
  v.l = h->nprotocols ? string_array_of(vm, (const char *const *)h->protocols, h->nprotocols)
        : string_array_of(vm, g_ssl_default_protocols, 2);
  (void)dvm_set_field(vm, p, "protocols", "[Ljava/lang/String;", v);
  v.l = string_array_of(vm, (const char *const *)h->alpn, h->nalpn);
  (void)dvm_set_field(vm, p, "applicationProtocols", "[Ljava/lang/String;", v);
  v.l = h->verify_host ? dvm_new_string(vm, "HTTPS") : 0;
  (void)dvm_set_field(vm, p, "endpointIdentificationAlgorithm", "Ljava/lang/String;", v);
  v.i = h->need_client_auth;
  (void)dvm_set_field(vm, p, "needClientAuth", "Z", v);
  v.i = h->want_client_auth;
  (void)dvm_set_field(vm, p, "wantClientAuth", "Z", v);
  (void)en;
  dvm_unpin(vm, p);
  RETL(p);
}

static bool x_hse_set_params(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  (void)nargs;
  (void)out;
  struct host_engine *h = host_engine_of(vm, self);
  if (!h) return false;
  dvm_ref p = ARG(0).l;
  if (!p) RETV();
  union dvm_value v = { 0 };
  char **l = NULL;
  int n;
  if (dvm_get_field(vm, p, "cipherSuites", "[Ljava/lang/String;", &v) && v.l) {
    n = string_array_take(vm, v.l, &l);
    host_engine_set_list(&h->ciphers, &h->nciphers, l, n);
  }
  if (dvm_get_field(vm, p, "protocols", "[Ljava/lang/String;", &v) && v.l) {
    n = string_array_take(vm, v.l, &l);
    host_engine_set_list(&h->protocols, &h->nprotocols, l, n);
  }
  if (dvm_get_field(vm, p, "applicationProtocols", "[Ljava/lang/String;", &v) && v.l) {
    n = string_array_take(vm, v.l, &l);
    host_engine_set_list(&h->alpn, &h->nalpn, l, n);
  }
  if (dvm_get_field(vm, p, "endpointIdentificationAlgorithm", "Ljava/lang/String;", &v))
    h->verify_host = v.l && !strcasecmp(dvm_string_utf8(vm, v.l), "HTTPS");
  if (dvm_get_field(vm, p, "needClientAuth", "Z", &v)) h->need_client_auth = v.i != 0;
  if (dvm_get_field(vm, p, "wantClientAuth", "Z", &v)) h->want_client_auth = v.i != 0;
  RETV();
}

static const struct rt_method rt_host_ssl_engine[] = {
  M("<init>", "()V", ssl_engine_init),
  M("<init>", "(Ljava/lang/String;I)V", ssl_engine_init),
  M("beginHandshake", "()V", x_hse_begin),
  M("getHandshakeStatus", "()Ljavax/net/ssl/SSLEngineResult$HandshakeStatus;", x_hse_hs_status),
  M("wrap", "([Ljava/nio/ByteBuffer;IILjava/nio/ByteBuffer;)Ljavax/net/ssl/SSLEngineResult;", x_hse_wrap),
  M("unwrap", "(Ljava/nio/ByteBuffer;[Ljava/nio/ByteBuffer;II)Ljavax/net/ssl/SSLEngineResult;", x_hse_unwrap),
  M("closeInbound", "()V", x_hse_close),
  M("closeOutbound", "()V", x_hse_close),
  M("isInboundDone", "()Z", x_hse_done),
  M("isOutboundDone", "()Z", x_hse_done),
  M("getDelegatedTask", "()Ljava/lang/Runnable;", x_hse_task),
  M("getSupportedProtocols", "()[Ljava/lang/String;", x_hse_supported),
  M("getSupportedCipherSuites", "()[Ljava/lang/String;", x_hse_supported),
  M("getEnabledProtocols", "()[Ljava/lang/String;", x_hse_enabled),
  M("getEnabledCipherSuites", "()[Ljava/lang/String;", x_hse_enabled),
  M("setEnabledProtocols", "([Ljava/lang/String;)V", x_hse_set_enabled),
  M("setEnabledCipherSuites", "([Ljava/lang/String;)V", x_hse_set_enabled),
  M("setUseClientMode", "(Z)V", x_hse_mode),
  M("getUseClientMode", "()Z", x_hse_mode),
  M("setNeedClientAuth", "(Z)V", x_hse_mode),
  M("getNeedClientAuth", "()Z", x_hse_mode),
  M("setWantClientAuth", "(Z)V", x_hse_mode),
  M("getWantClientAuth", "()Z", x_hse_mode),
  M("setEnableSessionCreation", "(Z)V", x_hse_mode),
  M("getEnableSessionCreation", "()Z", x_hse_mode),
  M("getApplicationProtocol", "()Ljava/lang/String;", x_hse_alpn),
  M("getHandshakeApplicationProtocol", "()Ljava/lang/String;", x_hse_alpn),
  M("getSession", "()Ljavax/net/ssl/SSLSession;", x_hse_session),
  M("getSSLParameters", "()Ljavax/net/ssl/SSLParameters;", x_hse_get_params),
  M("setSSLParameters", "(Ljavax/net/ssl/SSLParameters;)V", x_hse_set_params),
  M_END,
};

/* SSLContext.createSSLEngine([host, port]) of the built-in context. */
static dvm_ref host_engine_new(struct dvm *vm, const char *host, int port)
{
  struct dvm_class *c = dvm__class_by_desc(vm, "Llunaria/HostSSLEngine;");
  dvm_ref e = c ? dvm_new_object(vm, c) : 0;
  struct host_engine *h = e ? host_engine_of(vm, e) : NULL;
  if (!h) return 0;
  if (host) snprintf(h->peer_host, sizeof h->peer_host, "%s", host);
  h->peer_port = host ? port : -1;
  return e;
}

static const struct rt_field rt_ssl_engine_fields[] = {
  { "peerHost", "Ljava/lang/String;" }, { "peerPort", "I" }, F_END,
};
static const struct rt_method rt_ssl_engine[] = {
  M("<init>", "()V", ssl_engine_init),
  M("<init>", "(Ljava/lang/String;I)V", ssl_engine_init),
  M("getPeerHost", "()Ljava/lang/String;", ssl_engine_peer),
  M("getPeerPort", "()I", ssl_engine_peer),
  M("wrap", "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;)Ljavax/net/ssl/SSLEngineResult;", ssl_engine_wrap),
  M("wrap", "([Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;)Ljavax/net/ssl/SSLEngineResult;", ssl_engine_wrap),
  M("unwrap", "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;)Ljavax/net/ssl/SSLEngineResult;", ssl_engine_unwrap),
  M("unwrap", "(Ljava/nio/ByteBuffer;[Ljava/nio/ByteBuffer;)Ljavax/net/ssl/SSLEngineResult;", ssl_engine_unwrap),
  M("getHandshakeSession", "()Ljavax/net/ssl/SSLSession;", ssl_engine_null),
  M_END,
};
