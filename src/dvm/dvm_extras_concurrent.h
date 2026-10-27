/* java.util.concurrent.locks.AbstractQueuedSynchronizer.
 *
 * Libraries build their own latches, locks and semaphores on it by overriding
 * tryAcquire/tryRelease (exclusive) or tryAcquireShared/tryReleaseShared and
 * then calling acquire/release.  The framework owns the synchronisation state
 * (an int) and the waiting; the subclass owns what the state means.  Waiting
 * uses the interpreter-lock channel keyed by the synchronizer, as
 * CountDownLatch does, and every change to the state wakes the waiters so each
 * re-runs its try method.  Threads are not queued in arrival order. */

static bool aqs_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "state", "I", (union dvm_value){ .i = 0 });
   (void)dvm_set_field(vm, self, "waiters", "I", (union dvm_value){ .i = 0 });
   RETV();
}

static bool aqs_get_state(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   union dvm_value v = { 0 };
   (void)args; (void)nargs;
   (void)dvm_get_field(vm, self, "state", "I", &v);
   RETI(v.i);
}

static bool aqs_set_state(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)out;
   (void)dvm_set_field(vm, self, "state", "I", ARG(0));
   dvm_gil_notify_for(self);
   RETV();
}

/* Threads run one at a time under the interpreter lock, so a read followed by
 * a write cannot be interleaved with another thread's. */
static bool aqs_cas_state(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "state", "I", &v);
   if (v.i != ARG(0).i) RETI(0);
   (void)dvm_set_field(vm, self, "state", "I", ARG(1));
   dvm_gil_notify_for(self);
   RETI(1);
}

/* The overridable steps throw until a subclass supplies them. */
static bool aqs_unsupported(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs; (void)out;
   dvm__throw(vm, "java/lang/UnsupportedOperationException", "%s", builtin_name());
   return false;
}

static bool aqs_waiters_adjust(struct dvm *vm, dvm_ref self, int delta)
{
   union dvm_value n = { 0 };
   (void)dvm_get_field(vm, self, "waiters", "I", &n);
   n.i += delta;
   return dvm_set_field(vm, self, "waiters", "I", n);
}

/* Calls the subclass's try method.  1 = succeeded, 0 = not yet, -1 = an
 * exception is pending. */
static int aqs_try(struct dvm *vm, dvm_ref self, const char *name, bool shared, int32_t arg)
{
   union dvm_value a = { .i = arg }, r = { 0 };
   if (!am_vcall(vm, self, name, shared ? "(I)I" : "(I)Z", &a, 1, &r)) return -1;
   return shared ? r.i >= 0 : r.i != 0;
}

/* Runs the try method until it succeeds, the deadline passes, or the VM stops.
 * deadline_ms == 0 waits without a deadline of its own.  Returns 1 acquired,
 * 0 timed out, -1 exception pending or the VM stopping. */
static int aqs_acquire(struct dvm *vm, dvm_ref self, bool shared, int32_t arg, uint64_t deadline_ms)
{
   const char *name = shared ? "tryAcquireShared" : "tryAcquire";
   int got = aqs_try(vm, self, name, shared, arg);
   if (got) return got;
   const uint64_t hard_deadline = dvm__now_ms() + DVM_BLOCK_MAX_MS;
   (void)aqs_waiters_adjust(vm, self, 1);
   for (;;) {
      if (dvm_is_stopping(vm)) { got = -1; break; }
      dvm__drain_for_wait(vm);
      got = aqs_try(vm, self, name, shared, arg);
      if (got) break;
      const uint64_t now = dvm__now_ms();
      if (deadline_ms && now >= deadline_ms) break;
      /* Nobody left that could release it: waiting out the clock would only
       * freeze the process, as for a latch nothing will count down. */
      if (!vm->npending && !dvm__other_threads_live()) break;
      if (now >= hard_deadline) break;
      uint64_t until = hard_deadline;
      if (deadline_ms && deadline_ms < until) until = deadline_ms;
      dvm_ref thread = dvm__current_thread(vm);
      dvm__thread_set_state(vm, thread, deadline_ms ? DVM_THREAD_TIMED_WAITING : DVM_THREAD_WAITING);
      dvm_gil_wait_for(vm, self, (unsigned)(until > now ? until - now : 1));
      dvm__thread_set_state(vm, thread, DVM_THREAD_RUNNABLE);
   }
   (void)aqs_waiters_adjust(vm, self, -1);
   return got;
}

static bool aqs_acquire_method(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)out;
   const bool shared = strstr(builtin_name(), "Shared") != NULL;
   return aqs_acquire(vm, self, shared, ARG(0).i, 0) >= 0;
}

static bool aqs_try_acquire_nanos(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   const bool shared = strstr(builtin_name(), "Shared") != NULL;
   int64_t ns = ARG(1).j;
   /* A zero or negative timeout is one attempt. */
   const uint64_t deadline = dvm__now_ms() + (ns > 0 ? (uint64_t)(ns / 1000000) + 1 : 0);
   const int got = aqs_acquire(vm, self, shared, ARG(0).i, ns > 0 ? deadline : dvm__now_ms());
   if (got < 0) return false;
   RETI(got);
}

static bool aqs_release_method(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   const bool shared = strstr(builtin_name(), "Shared") != NULL;
   const int done = aqs_try(vm, self, shared ? "tryReleaseShared" : "tryRelease", false, ARG(0).i);
   if (done < 0) return false;
   if (done) dvm_gil_notify_for(self);
   RETI(done);
}

static bool aqs_has_queued(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   union dvm_value n = { 0 };
   (void)args; (void)nargs;
   (void)dvm_get_field(vm, self, "waiters", "I", &n);
   RETI(n.i > 0);
}

static bool aqs_queue_length(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   union dvm_value n = { 0 };
   (void)args; (void)nargs;
   (void)dvm_get_field(vm, self, "waiters", "I", &n);
   RETI(n.i);
}

static const struct rt_field rt_aqs_fields[] = {
   { "state", "I" }, { "waiters", "I" }, F_END
};

static const struct rt_method rt_aqs[] = {
   M("<init>", "()V", aqs_init),
   M("getState", "()I", aqs_get_state),
   M("setState", "(I)V", aqs_set_state),
   M("compareAndSetState", "(II)Z", aqs_cas_state),
   M("tryAcquire", "(I)Z", aqs_unsupported),
   M("tryRelease", "(I)Z", aqs_unsupported),
   M("tryAcquireShared", "(I)I", aqs_unsupported),
   M("tryReleaseShared", "(I)Z", aqs_unsupported),
   M("isHeldExclusively", "()Z", aqs_unsupported),
   M("acquire", "(I)V", aqs_acquire_method),
   M("acquireInterruptibly", "(I)V", aqs_acquire_method),
   M("acquireShared", "(I)V", aqs_acquire_method),
   M("acquireSharedInterruptibly", "(I)V", aqs_acquire_method),
   M("tryAcquireNanos", "(IJ)Z", aqs_try_acquire_nanos),
   M("tryAcquireSharedNanos", "(IJ)Z", aqs_try_acquire_nanos),
   M("release", "(I)Z", aqs_release_method),
   M("releaseShared", "(I)Z", aqs_release_method),
   M("hasQueuedThreads", "()Z", aqs_has_queued),
   M("getQueueLength", "()I", aqs_queue_length),
   M_END,
};
