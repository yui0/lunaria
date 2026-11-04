/* IntentSender and the Activity calls that start one.
 *
 * A PendingIntent hands out an IntentSender -- the capability to run it -- and
 * an Activity starts it with startIntentSender().  Here the sender names its
 * PendingIntent; starting it does what sending the PendingIntent does, from the
 * Activity that asked (an activity PendingIntent opens its screen on top of
 * that Activity, with the fill-in Intent's extras merged in).
 *
 * Included once, after the pending-intent and activity-stack code. */

static const struct rt_field rt_intent_sender_fields[] = {
   { "pending", "Landroid/app/PendingIntent;" }, F_END,
};

static bool pending_getIntentSender(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/content/IntentSender;");
   dvm_ref sender = c ? dvm_new_object(vm, c) : 0;
   if (!sender) { dvm__throw(vm, "java/lang/OutOfMemoryError", "IntentSender"); return false; }
   union dvm_value p = { .l = self };
   (void)dvm_set_field(vm, sender, "pending", "Landroid/app/PendingIntent;", p);
   RETL(sender);
}
static bool intent_sender_creator_package(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   dvm_ref token = pending_field(vm, pending_field(vm, self, "pending", "Landroid/app/PendingIntent;"),
                                 "token", "Landroid/app/PendingIntent;");
   RETL(token ? pending_field(vm, token, "creator", "Ljava/lang/String;") : 0);
}
static const struct rt_method rt_intent_sender[] = {
   M("getCreatorPackage", "()Ljava/lang/String;", intent_sender_creator_package),
   M("getTargetPackage", "()Ljava/lang/String;", intent_sender_creator_package),
   M_END,
};
static const struct rt_method rt_x_pending_intent_sender[] = {
   M("getIntentSender", "()Landroid/content/IntentSender;", pending_getIntentSender), M_END,
};

/* startIntentSender(sender, fillIn, flagsMask, flagsValues, extraFlags[, options]) and
 * startIntentSenderForResult(sender, requestCode, fillIn, ...). */
static bool act_startIntentSender(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)nargs;
   const bool for_result = !strcmp(dvm__builtin_method()->name, "startIntentSenderForResult");
   dvm_ref sender = args[0].l, fill_in = for_result ? args[2].l : args[1].l;
   const int32_t code = for_result ? args[1].i : -1;
   if (!sender) { dvm__throw(vm, "java/lang/NullPointerException", "IntentSender"); return false; }
   dvm_ref token = pending_field(vm, pending_field(vm, sender, "pending", "Landroid/app/PendingIntent;"),
                                 "token", "Landroid/app/PendingIntent;");
   if (!token || rt_get_int(vm, token, "cancelled")) {
      dvm__throw(vm, "android/content/IntentSender$SendIntentException", "IntentSender is cancelled");
      return false;
   }
   if (rt_get_int(vm, token, "type") != PI_ACTIVITY) {
      if (!pending_send_token(vm, token, 0)) return false;
      RETV();
   }
   dvm_ref intent = dvm_new_object(vm, dvm_find_class(vm, "android/content/Intent"));
   if (!intent) return false;
   dvm_pin(vm, intent);
   union dvm_value ignored = { 0 };
   bool ok = intent_copy_into(vm, intent, pending_field(vm, token, "intent", "Landroid/content/Intent;"), false);
   /* FLAG_IMMUTABLE ignores what the sender fills in. */
   if (ok && fill_in && !((uint32_t)rt_get_int(vm, token, "flags") & UINT32_C(0x04000000)))
      ok = am_vcall(vm, intent, "putExtras", "(Landroid/content/Intent;)Landroid/content/Intent;",
                    &(union dvm_value){ .l = fill_in }, 1, &ignored);
   ok = ok && act_start_common(vm, self, intent, true, for_result, code);
   dvm_unpin(vm, intent);
   if (!ok) return false;
   RETV();
}

/* The Activity that started this one for a result; null when it was not. */
static bool act_getCallingActivity(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   for (int i = 0; i < g_nacts; ++i)
      if (g_acts[i].activity == self) {
         if (!g_acts[i].for_result || !g_acts[i].caller) break;
         return am_vcall(vm, g_acts[i].caller, "getComponentName", "()Landroid/content/ComponentName;", NULL, 0, out);
      }
   RETL(0);
}

static const struct rt_method rt_x_activity_sender[] = {
   M("startIntentSender", "(Landroid/content/IntentSender;Landroid/content/Intent;III)V", act_startIntentSender),
   M("startIntentSender", "(Landroid/content/IntentSender;Landroid/content/Intent;IIILandroid/os/Bundle;)V", act_startIntentSender),
   M("startIntentSenderForResult", "(Landroid/content/IntentSender;ILandroid/content/Intent;III)V", act_startIntentSender),
   M("startIntentSenderForResult", "(Landroid/content/IntentSender;ILandroid/content/Intent;IIILandroid/os/Bundle;)V", act_startIntentSender),
   M("getCallingActivity", "()Landroid/content/ComponentName;", act_getCallingActivity),
   /* The options menu is rebuilt the next time it is shown; a screen without a menu bar has nothing to redo. */
   M("invalidateOptionsMenu", "()V", nop_void),
   M_END,
};
