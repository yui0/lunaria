/* SPDX-License-Identifier: MPL-2.0 */
/* System library identities for hosts using the emulator's guest ABI.
 * pthread objects, libc calls, FILE objects, errno and libc data symbols are
 * resolved in arm_exec.cpp. Host CRT wrappers cannot share their layouts or
 * scheduler. These libraries provide DT_NEEDED identities without exporting
 * guest functions that could fall through to incompatible native calls. */
#ifdef LUNARIA_SVC_LIBC
int lunaria_libc_runtime_is_svc_backed(void)
#else
int lunaria_pthread_runtime_is_svc_backed(void)
#endif
{
   return 1;
}
