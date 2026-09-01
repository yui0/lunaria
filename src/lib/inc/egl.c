
#include <dlfcn.h>
#include <string.h>
#include <EGL/egl.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "lunaria_os.h"

struct ANativeWindow {
   char header[4];
   GLFWwindow *glfw;
};

EGLSurface
eglCreateWindowSurface(EGLDisplay display, EGLConfig config, NativeWindowType native_window, EGLint const *attrib_list)
{
   static union { EGLSurface (*fun)(EGLDisplay, EGLConfig, NativeWindowType, EGLint const*); void *ptr; } orig;
   if (!orig.ptr) orig.ptr = dlsym(RTLD_NEXT, "eglCreateWindowSurface");
   struct ANativeWindow *window = (struct ANativeWindow*)native_window;
   NativeWindowType host_window = native_window;
   if (!memcmp(window->header, "andr", sizeof(window->header)))
      host_window = (NativeWindowType)luna_os_native_window(window->glfw);
   return orig.fun(display, config, host_window, attrib_list);
}
