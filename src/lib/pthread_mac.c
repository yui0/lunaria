/*
 * The macOS/AArch64 runtime intentionally exports no host pthread wrappers.
 * Guest pthread and semaphore entry points are resolved to SVCs in
 * arm_exec.cpp, where their objects, wait queues and scheduling semantics are
 * implemented in the guest ABI.  Treating a 4-byte bionic pthread_key_t as
 * Darwin's 8-byte pthread_key_t (the old host shim) corrupts adjacent guest
 * memory; keeping a second host scheduler also defeats direct SVC wakeups.
 *
 * This Mach-O image exists only as the Android DT_NEEDED identity for code
 * paths that enumerate system libraries.  There is deliberately no fallback
 * from an unresolved guest pthread symbol to Darwin pthread.
 */
int lunaria_pthread_runtime_is_svc_backed(void)
{
   return 1;
}
