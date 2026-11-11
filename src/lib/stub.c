/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Single build driver for small Android runtime stubs (liblog, libEGL, …).
 * Compile with exactly one -DLUNARIA_STUB_<NAME> to select the module.
 */

#if defined(LUNARIA_STUB_LOG)
# include "inc/log.c"
#elif defined(LUNARIA_STUB_EGL) || defined(LUNARIA_STUB_GLESV3)
# include "inc/gl.c"
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
#elif defined(LUNARIA_STUB_ARTBASE)
/* Guest-side ART file transport. The VM parses DEX on the host, while file
 * mapping and reading run through the Android runtime's own import table. */
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ssize_t;
extern int open(const char *, int, ...), close(int), *__errno(void);
extern long lseek(int, long, int);
extern ssize_t read(int, void *, size_t);
extern void *mmap(void *, size_t, int, int, int, long);
extern int munmap(void *, size_t);
extern void *malloc(size_t);
extern void free(void *);

struct runtime_file {
  void *data;
  size_t size;
  int mapped;
};

int lunaria_runtime_file_open(const char *path, struct runtime_file *file)
{
  file->data = 0;
  file->size = 0;
  file->mapped = 0;
  int fd = open(path, 0x80000); /* O_RDONLY | O_CLOEXEC */
  if (fd < 0) return -1;
  long length = lseek(fd, 0, 2);
  if (length <= 0 || lseek(fd, 0, 0) < 0) {
    close(fd);
    return -1;
  }
  size_t size = (size_t)length;
  void *data = mmap(0, size, 1, 2, fd, 0); /* PROT_READ, MAP_PRIVATE */
  if (data != (void *)-1) {
    file->data = data;
    file->size = size;
    file->mapped = 1;
    close(fd);
    return 0;
  }
  data = malloc(size);
  if (!data) {
    close(fd);
    return -1;
  }
  size_t done = 0;
  while (done < size) {
    ssize_t count = read(fd, (char *)data + done, size - done);
    if (count < 0 && *__errno() == 4) continue; /* EINTR */
    if (count <= 0) {
      free(data);
      close(fd);
      return -1;
    }
    done += (size_t)count;
  }
  close(fd);
  file->data = data;
  file->size = size;
  return 0;
}

void lunaria_runtime_file_close(struct runtime_file *file)
{
  if (file->mapped) munmap(file->data, file->size);
  else free(file->data);
  file->data = 0;
  file->size = 0;
  file->mapped = 0;
}

#elif defined(LUNARIA_STUB_ART_IMAGE)
/* A zygote child maps libart.so, libdexfile.so and
 * libnativeloader.so before app code runs. Process-protection SDKs inspect
 * their /proc/self/maps entries and ELF headers. These identities provide
 * readable ELF images; Java execution is handled by Lunaria's DVM.
 * Built once per library with ART_IMAGE_SYMBOL naming its exported marker. */
__attribute__((visibility("default")))
int ART_IMAGE_SYMBOL(void)
{
  return 0;
}
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
