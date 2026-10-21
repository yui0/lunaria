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
