/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Single build driver for small Android runtime stubs (liblog, libEGL, …).
 * Compile with exactly one -DLUNARIA_STUB_<NAME> to select the module.
 */

#if defined(LUNARIA_STUB_LOG)
# include "inc/log.c"
#elif defined(LUNARIA_STUB_EGL)
# include "inc/egl.c"
#elif defined(LUNARIA_STUB_MEDIANDK)
# include "inc/mediandk.c"
#elif defined(LUNARIA_STUB_MATH)
# include "inc/math.c"
#elif defined(LUNARIA_STUB_ZLIB)
# include "inc/zlib.c"
#elif defined(LUNARIA_STUB_ANDROID)
# include "inc/android.c"
#elif defined(LUNARIA_STUB_OPENSLES)
# include "inc/opensles.c"
#elif defined(LUNARIA_STUB_GLESV3)
# include "inc/glesv3.c"
#elif defined(LUNARIA_STUB_SVC_RUNTIME)
/* System library identities for hosts using the emulator's guest ABI.
 * pthread objects, libc calls, FILE objects, errno and libc data symbols are
 * resolved in arm_exec.cpp. Host CRT wrappers cannot share their layouts or
 * scheduler. These libraries provide DT_NEEDED identities without exporting
 * guest functions that could fall through to incompatible native calls.
 * Built once as libpthread.so and once (with -DLUNARIA_SVC_LIBC) as libc.so. */
# ifdef LUNARIA_SVC_LIBC
int lunaria_libc_runtime_is_svc_backed(void)
# else
int lunaria_pthread_runtime_is_svc_backed(void)
# endif
{
   return 1;
}
#else
# error "Define one of the LUNARIA_STUB_* runtime modules"
#endif
