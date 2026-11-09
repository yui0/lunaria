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
static const struct rt_method rt_database_utils[] = {
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
