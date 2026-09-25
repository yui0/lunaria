/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Vulkan for A64 guests: the guest's libvulkan.so is the host's.
 *
 * Guest and host share an address space (identity VA) and AArch64 and x86-64
 * lay out every Vulkan structure the same way (LP64, natural alignment), so
 * almost every command is a straight call: its arguments are read out of the
 * AAPCS64 registers and stack, and the host function is called with them.
 * Each command gets a host-call trampoline (see HostCallFn in arm_exec.h);
 * the table of commands and of which parameters are floats is generated from
 * vk.xml (scripts/gen_vk_commands.py -> luna_vulkan_cmds.h).
 *
 * What cannot be passed through is handled here:
 *  - VkAllocationCallbacks are guest code the driver cannot call: always null.
 *  - Debug callbacks likewise: messengers are accepted and never called.
 *  - Presentation.  An Android surface is an ANativeWindow the host has never
 *    heard of, and the host window belongs to the compositor.  So surfaces and
 *    swapchains are emulated: swapchain images are ordinary images, and a
 *    present copies the image to host memory and hands it to the compositor
 *    (luna_comp_submit_pixels).
 *  - The extension lists: the host's platform-surface extensions become
 *    VK_KHR_android_surface, and what the host cannot back is not offered.
 *  - Memory the guest maps is declared to the guest's address-space table, so
 *    the SVCs that check a guest pointer accept it.
 *
 * Off unless LUNARIA_VULKAN=1: a title that finds no Vulkan uses GLES, and
 * that path is the proven one.
 */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_exec.h"
#include "luna_compositor.h"

/* ---- the command table -------------------------------------------------- */

enum { LVK_GLOBAL, LVK_INSTANCE, LVK_DEVICE };
enum { LVK_RET_VOID, LVK_RET_RESULT, LVK_RET_PFN, LVK_RET_INT };

struct lvk_cmd {
   const char *name;
   uint8_t nparams;
   uint32_t float_mask;   /* bit n: parameter n is a float by value       */
   int8_t alloc;          /* index of pAllocator, or -1                   */
   uint8_t level;
   uint8_t ret;
};

#include "luna_vulkan_cmds.h"

#define NCMD (sizeof lvk_cmds / sizeof lvk_cmds[0])

/* The generic call below relies on the host ABI assigning integer and float
 * arguments to separate register files and the remaining integers to the
 * stack in order — SysV x86-64 and Linux AArch64 both do. */
#if (defined(__x86_64__) && !defined(_WIN32)) || \
    (defined(__aarch64__) && !defined(__APPLE__))
#define LVK_ABI_OK 1
#else
#define LVK_ABI_OK 0
#endif

/* ---- state -------------------------------------------------------------- */

static void *g_lib;

/* dlsym() for a function: POSIX's own idiom, which ISO C's rule against
 * object-to-function pointer casts does not reach. */
static PFN_vkVoidFunction lib_fn(const char *name)
{
   PFN_vkVoidFunction f;
   void *p = dlsym(g_lib, name);
   memcpy(&f, &p, sizeof f);
   return f;
}
static PFN_vkGetInstanceProcAddr g_gipa;
static bool g_ok;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static _Atomic(VkInstance) g_instance;
static _Atomic(PFN_vkVoidFunction) g_fn[NCMD];  /* host function, resolved */
static _Atomic(uint64_t) g_tramp[NCMD];         /* guest trampoline        */
static HostCallFn g_special[NCMD];              /* our own implementation  */
static bool g_queue_op[NCMD];                   /* needs g_queue_mu        */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
/* vkQueueSubmit and friends require the queue to be externally synchronised.
 * The emulated acquire and present submit on the application's queues from
 * whatever thread calls them, so every queue operation takes this. */
static pthread_mutex_t g_queue_mu = PTHREAD_MUTEX_INITIALIZER;

/* Name -> command index: FNV-1a, open addressing, built once. */
#define HASH_SIZE 2048u
static uint16_t g_hash[HASH_SIZE];   /* index + 1, 0 = empty */

static uint32_t fnv1a(const char *s)
{
   uint32_t h = 2166136261u;
   while (*s) {
      h ^= (uint8_t)*s++;
      h *= 16777619u;
   }
   return h;
}

static int cmd_index(const char *name)
{
   if (!name) return -1;
   const uint32_t h = fnv1a(name);
   for (uint32_t i = 0; i < HASH_SIZE; ++i) {
      const uint16_t e = g_hash[(h + i) & (HASH_SIZE - 1u)];
      if (!e) return -1;
      if (!strcmp(lvk_cmds[e - 1].name, name)) return e - 1;
   }
   return -1;
}

static PFN_vkVoidFunction host_fn_at(int i)
{
   PFN_vkVoidFunction f = atomic_load_explicit(&g_fn[i], memory_order_acquire);
   if (f) return f;
   VkInstance inst = lvk_cmds[i].level == LVK_GLOBAL
                        ? VK_NULL_HANDLE : atomic_load(&g_instance);
   f = g_gipa(inst, lvk_cmds[i].name);
   if (!f) f = lib_fn(lvk_cmds[i].name);
   if (f) atomic_store_explicit(&g_fn[i], f, memory_order_release);
   return f;
}

static PFN_vkVoidFunction host_fn(const char *name)
{
   const int i = cmd_index(name);
   return i < 0 ? NULL : host_fn_at(i);
}

#define HOST(name) ((PFN_##name)host_fn(#name))

static uint64_t vk_ret(VkResult r) { return (uint64_t)(int64_t)r; }

/* ---- the generic call --------------------------------------------------- */

typedef uint64_t (*lvk_any_fn)(float, float, float, float, float, float,
                               float, float,
                               uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t);

static void vk_generic(HostCallArgs *a, uintptr_t idx)
{
   const struct lvk_cmd *c = &lvk_cmds[idx];
   PFN_vkVoidFunction fn = host_fn_at((int)idx);
   if (!fn) {
      static _Atomic(int) said;
      if (atomic_fetch_add(&said, 1) < 16)
         fprintf(stderr, "[vulkan] %s: the host has no such function\n", c->name);
      a->ret = c->ret == LVK_RET_RESULT
                  ? vk_ret(VK_ERROR_EXTENSION_NOT_PRESENT) : 0;
      return;
   }
   float f[8] = { 0 };
   uint64_t iv[16] = { 0 };
   const uint64_t *stack = (const uint64_t *)(uintptr_t)a->sp;
   unsigned nf = 0, ni = 0, xi = 0, si = 0;
   for (unsigned p = 0; p < c->nparams; ++p) {
      if ((c->float_mask >> p) & 1u) {
         const uint32_t bits = (uint32_t)a->v[nf];
         memcpy(&f[nf++], &bits, 4);
         continue;
      }
      uint64_t v = xi < 8 ? a->x[xi] : stack[si++];
      ++xi;
      if ((int)p == c->alloc) v = 0;
      iv[ni++] = v;
   }
   if (g_queue_op[idx]) pthread_mutex_lock(&g_queue_mu);
   a->ret = ((lvk_any_fn)fn)(f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                             iv[0], iv[1], iv[2], iv[3], iv[4], iv[5],
                             iv[6], iv[7], iv[8], iv[9], iv[10], iv[11],
                             iv[12], iv[13], iv[14], iv[15]);
   if (g_queue_op[idx]) pthread_mutex_unlock(&g_queue_mu);
}

/* ---- trampolines and proc addresses ------------------------------------ */

static uint64_t tramp_at(int i)
{
   uint64_t t = atomic_load_explicit(&g_tramp[i], memory_order_acquire);
   if (t) return t;
   pthread_mutex_lock(&g_mu);
   t = atomic_load_explicit(&g_tramp[i], memory_order_relaxed);
   if (!t) {
      const unsigned flags = HOSTCALL_LOCKFREE |
                             (lvk_cmds[i].float_mask ? HOSTCALL_FLOATS : 0u);
      t = luna_host_call_tramp(lvk_cmds[i].name,
                               g_special[i] ? g_special[i] : vk_generic,
                               (uintptr_t)i, flags);
      atomic_store_explicit(&g_tramp[i], t, memory_order_release);
   }
   pthread_mutex_unlock(&g_mu);
   return t;
}

/* vkGet{Instance,Device}ProcAddr: a trampoline for what the host has, and
 * null for what it does not — the answer the application acts on. */
static uint64_t proc_addr(VkInstance inst, VkDevice dev, const char *name)
{
   const int i = cmd_index(name);
   if (i < 0) return 0;
   /* vkGetDeviceProcAddr answers only for device-level commands. */
   if (dev && lvk_cmds[i].level != LVK_DEVICE) return 0;
   if (!g_special[i]) {
      PFN_vkVoidFunction f;
      if (dev) {
         PFN_vkGetDeviceProcAddr gdpa = HOST(vkGetDeviceProcAddr);
         f = gdpa ? gdpa(dev, name) : NULL;
      } else {
         f = g_gipa(inst, name);
      }
      if (!f) return 0;
   }
   return tramp_at(i);
}

static void h_get_instance_proc_addr(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   a->ret = proc_addr((VkInstance)(uintptr_t)a->x[0], VK_NULL_HANDLE,
                      (const char *)(uintptr_t)a->x[1]);
}

static void h_get_device_proc_addr(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   a->ret = proc_addr(VK_NULL_HANDLE, (VkDevice)(uintptr_t)a->x[0],
                      (const char *)(uintptr_t)a->x[1]);
}

/* ---- enumeration -------------------------------------------------------- */

/* The two-call idiom: a count, then up to that many elements. */
static VkResult fill_array(uint32_t *count, void *dst, const void *src,
                           uint32_t n, size_t elem)
{
   if (!count) return VK_ERROR_INITIALIZATION_FAILED;
   if (!dst) {
      *count = n;
      return VK_SUCCESS;
   }
   const uint32_t k = *count < n ? *count : n;
   memcpy(dst, src, (size_t)k * elem);
   *count = k;
   return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static bool name_in(const char *name, const char *const *list)
{
   for (; *list; ++list)
      if (!strcmp(name, *list)) return true;
   return false;
}

/* Instance extensions an Android device has and this bridge can back. */
static const char *const k_instance_ext[] = {
   "VK_KHR_surface",
   "VK_KHR_get_physical_device_properties2",
   "VK_KHR_get_surface_capabilities2",
   "VK_KHR_external_memory_capabilities",
   "VK_KHR_external_semaphore_capabilities",
   "VK_KHR_external_fence_capabilities",
   "VK_KHR_device_group_creation",
   "VK_EXT_debug_utils",
   "VK_EXT_debug_report",
   NULL,
};

/* Device extensions that belong to the host's own display, or to a
 * swapchain feature the emulated swapchain does not have. */
static const char *const k_device_ext_drop[] = {
   "VK_KHR_display_swapchain", "VK_EXT_display_control",
   "VK_KHR_present_id", "VK_KHR_present_wait",
   "VK_KHR_present_id2", "VK_KHR_present_wait2",
   "VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1",
   "VK_KHR_swapchain_mutable_format", "VK_EXT_full_screen_exclusive",
   "VK_KHR_incremental_present", "VK_KHR_shared_presentable_image",
   "VK_EXT_hdr_metadata", "VK_AMD_display_native_hdr",
   "VK_EXT_physical_device_drm", "VK_NV_present_barrier",
   "VK_EXT_present_mode_fifo_latest_ready", "VK_GOOGLE_display_timing",
   NULL,
};

/* The host's instance extensions, malloc'ed; *n set. */
static VkExtensionProperties *host_instance_ext(uint32_t *n)
{
   *n = 0;
   PFN_vkEnumerateInstanceExtensionProperties f =
      HOST(vkEnumerateInstanceExtensionProperties);
   if (!f || f(NULL, n, NULL) != VK_SUCCESS) return NULL;
   VkExtensionProperties *p = calloc(*n + 1u, sizeof *p);
   if (p && f(NULL, n, p) < 0) { free(p); p = NULL; *n = 0; }
   return p;
}

static VkExtensionProperties *host_device_ext(VkPhysicalDevice pd, uint32_t *n)
{
   *n = 0;
   PFN_vkEnumerateDeviceExtensionProperties f =
      HOST(vkEnumerateDeviceExtensionProperties);
   if (!f || f(pd, NULL, n, NULL) != VK_SUCCESS) return NULL;
   VkExtensionProperties *p = calloc(*n + 2u, sizeof *p);
   if (p && f(pd, NULL, n, p) < 0) { free(p); p = NULL; *n = 0; }
   return p;
}

static bool ext_listed(const VkExtensionProperties *p, uint32_t n,
                       const char *name)
{
   for (uint32_t i = 0; i < n; ++i)
      if (!strcmp(p[i].extensionName, name)) return true;
   return false;
}

static void h_enum_instance_ext(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   if (a->x[0]) {   /* pLayerName: this device has no layers */
      a->ret = vk_ret(VK_ERROR_LAYER_NOT_PRESENT);
      return;
   }
   uint32_t n = 0, m = 0;
   VkExtensionProperties *host = host_instance_ext(&n);
   VkExtensionProperties out[32];
   for (uint32_t i = 0; i < n && m < 31; ++i)
      if (name_in(host[i].extensionName, k_instance_ext)) out[m++] = host[i];
   free(host);
   memset(&out[m], 0, sizeof out[m]);
   strcpy(out[m].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
   out[m++].specVersion = VK_KHR_ANDROID_SURFACE_SPEC_VERSION;
   a->ret = vk_ret(fill_array((uint32_t *)(uintptr_t)a->x[1],
                              (void *)(uintptr_t)a->x[2], out, m, sizeof *out));
}

static void h_enum_instance_layers(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   uint32_t *count = (uint32_t *)(uintptr_t)a->x[0];
   if (count) *count = 0;
   a->ret = vk_ret(VK_SUCCESS);
}

static void h_enum_device_layers(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   uint32_t *count = (uint32_t *)(uintptr_t)a->x[1];
   if (count) *count = 0;
   a->ret = vk_ret(VK_SUCCESS);
}

static void h_enum_device_ext(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)a->x[0];
   if (a->x[1]) {
      a->ret = vk_ret(VK_ERROR_LAYER_NOT_PRESENT);
      return;
   }
   uint32_t n = 0, m = 0;
   VkExtensionProperties *host = host_device_ext(pd, &n);
   if (!host) {
      a->ret = vk_ret(VK_ERROR_INITIALIZATION_FAILED);
      return;
   }
   for (uint32_t i = 0; i < n; ++i)
      if (!name_in(host[i].extensionName, k_device_ext_drop)) host[m++] = host[i];
   /* The emulated swapchain needs nothing of the host's. */
   if (!ext_listed(host, m, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
      memset(&host[m], 0, sizeof host[m]);
      strcpy(host[m].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
      host[m++].specVersion = VK_KHR_SWAPCHAIN_SPEC_VERSION;
   }
   a->ret = vk_ret(fill_array((uint32_t *)(uintptr_t)a->x[2],
                              (void *)(uintptr_t)a->x[3], host, m, sizeof *host));
   free(host);
}

/* ---- instance and device ------------------------------------------------ */

/* pNext entries whose contents are guest code (debug callbacks), unlinked
 * for the duration of one call and put back after it. */
struct unlinked { VkBaseOutStructure *prev; VkBaseOutStructure *node; };

static int unlink_callbacks(const void **head, struct unlinked *u, int max)
{
   int n = 0;
   VkBaseOutStructure *prev = NULL;
   VkBaseOutStructure *cur = (VkBaseOutStructure *)*head;
   while (cur && n < max) {
      VkBaseOutStructure *next = cur->pNext;
      if (cur->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT ||
          cur->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT) {
         u[n].prev = prev;
         u[n].node = cur;
         ++n;
         if (prev) prev->pNext = next;
         else *head = next;
      } else {
         prev = cur;
      }
      cur = next;
   }
   return n;
}

static void relink(struct unlinked *u, int n)
{
   /* In reverse, so each node goes back between the two it came from. */
   for (int i = n - 1; i >= 0; --i)
      if (u[i].prev) u[i].prev->pNext = u[i].node;
}

static void h_create_instance(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   const VkInstanceCreateInfo *ci = (const VkInstanceCreateInfo *)(uintptr_t)a->x[0];
   VkInstance *out = (VkInstance *)(uintptr_t)a->x[2];
   if (!ci || !out) {
      a->ret = vk_ret(VK_ERROR_INITIALIZATION_FAILED);
      return;
   }
   uint32_t n = 0;
   VkExtensionProperties *host = host_instance_ext(&n);
   const char *names[64];
   uint32_t k = 0;
   for (uint32_t i = 0; i < ci->enabledExtensionCount && k < 64; ++i) {
      const char *e = ci->ppEnabledExtensionNames[i];
      if (!strcmp(e, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) continue;
      if (ext_listed(host, n, e)) names[k++] = e;
      else fprintf(stderr, "[vulkan] vkCreateInstance: %s dropped (not on the host)\n", e);
   }
   free(host);
   VkInstanceCreateInfo c = *ci;
   c.enabledExtensionCount = k;
   c.ppEnabledExtensionNames = names;
   c.enabledLayerCount = 0;
   c.ppEnabledLayerNames = NULL;
   struct unlinked u[8];
   const int nu = unlink_callbacks(&c.pNext, u, 8);
   PFN_vkCreateInstance create = HOST(vkCreateInstance);
   const VkResult r = create ? create(&c, NULL, out) : VK_ERROR_INITIALIZATION_FAILED;
   relink(u, nu);
   if (r == VK_SUCCESS) atomic_store(&g_instance, *out);
   fprintf(stderr, "[vulkan] vkCreateInstance -> %d (%u extensions)\n", (int)r, k);
   a->ret = vk_ret(r);
}

static void h_destroy_instance(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkInstance inst = (VkInstance)(uintptr_t)a->x[0];
   PFN_vkDestroyInstance destroy = HOST(vkDestroyInstance);
   if (destroy && inst) destroy(inst, NULL);
   VkInstance cur = inst;
   atomic_compare_exchange_strong(&g_instance, &cur, VK_NULL_HANDLE);
}

/* What the present path needs to know about devices and queues. */
#define MAX_DEVICES 8
#define MAX_QUEUES 64
static struct { VkDevice dev; VkPhysicalDevice pd; VkQueue queue; } g_devs[MAX_DEVICES];
static struct { VkQueue q; VkDevice dev; uint32_t family; } g_queues[MAX_QUEUES];

static VkPhysicalDevice pd_of(VkDevice dev)
{
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   pthread_mutex_lock(&g_mu);
   for (int i = 0; i < MAX_DEVICES; ++i)
      if (g_devs[i].dev == dev) { pd = g_devs[i].pd; break; }
   pthread_mutex_unlock(&g_mu);
   return pd;
}

static VkQueue queue_of(VkDevice dev)
{
   VkQueue q = VK_NULL_HANDLE;
   pthread_mutex_lock(&g_mu);
   for (int i = 0; i < MAX_DEVICES; ++i)
      if (g_devs[i].dev == dev) { q = g_devs[i].queue; break; }
   pthread_mutex_unlock(&g_mu);
   return q;
}

static uint32_t family_of(VkQueue q)
{
   uint32_t f = 0;
   pthread_mutex_lock(&g_mu);
   for (int i = 0; i < MAX_QUEUES; ++i)
      if (g_queues[i].q == q) { f = g_queues[i].family; break; }
   pthread_mutex_unlock(&g_mu);
   return f;
}

static bool family_has_graphics(VkPhysicalDevice pd, uint32_t family)
{
   PFN_vkGetPhysicalDeviceQueueFamilyProperties props =
      HOST(vkGetPhysicalDeviceQueueFamilyProperties);
   if (!props || !pd) return false;
   uint32_t n = 0;
   props(pd, &n, NULL);
   if (family >= n || n > 64) return false;
   VkQueueFamilyProperties p[64];
   props(pd, &n, p);
   return (p[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
}

static void h_create_device(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)a->x[0];
   const VkDeviceCreateInfo *ci = (const VkDeviceCreateInfo *)(uintptr_t)a->x[1];
   VkDevice *out = (VkDevice *)(uintptr_t)a->x[3];
   if (!ci || !out) {
      a->ret = vk_ret(VK_ERROR_INITIALIZATION_FAILED);
      return;
   }
   uint32_t n = 0;
   VkExtensionProperties *host = host_device_ext(pd, &n);
   const char *names[256];
   uint32_t k = 0;
   for (uint32_t i = 0; i < ci->enabledExtensionCount && k < 256; ++i) {
      const char *e = ci->ppEnabledExtensionNames[i];
      if (ext_listed(host, n, e)) names[k++] = e;
      else fprintf(stderr, "[vulkan] vkCreateDevice: %s dropped (not on the host)\n", e);
   }
   free(host);
   VkDeviceCreateInfo c = *ci;
   c.enabledExtensionCount = k;
   c.ppEnabledExtensionNames = names;
   c.enabledLayerCount = 0;
   c.ppEnabledLayerNames = NULL;
   PFN_vkCreateDevice create = HOST(vkCreateDevice);
   const VkResult r = create ? create(pd, &c, NULL, out) : VK_ERROR_INITIALIZATION_FAILED;
   if (r == VK_SUCCESS) {
      pthread_mutex_lock(&g_mu);
      for (int i = 0; i < MAX_DEVICES; ++i)
         if (!g_devs[i].dev) {
            g_devs[i].dev = *out;
            g_devs[i].pd = pd;
            g_devs[i].queue = VK_NULL_HANDLE;
            break;
         }
      pthread_mutex_unlock(&g_mu);
   }
   fprintf(stderr, "[vulkan] vkCreateDevice -> %d (%u extensions)\n", (int)r, k);
   a->ret = vk_ret(r);
}

static void h_destroy_device(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkDevice dev = (VkDevice)(uintptr_t)a->x[0];
   PFN_vkDestroyDevice destroy = HOST(vkDestroyDevice);
   if (destroy && dev) destroy(dev, NULL);
   pthread_mutex_lock(&g_mu);
   for (int i = 0; i < MAX_DEVICES; ++i)
      if (g_devs[i].dev == dev) memset(&g_devs[i], 0, sizeof g_devs[i]);
   for (int i = 0; i < MAX_QUEUES; ++i)
      if (g_queues[i].dev == dev) memset(&g_queues[i], 0, sizeof g_queues[i]);
   pthread_mutex_unlock(&g_mu);
}

static void note_queue(VkDevice dev, uint32_t family, VkQueue q)
{
   if (!q) return;
   const bool graphics = family_has_graphics(pd_of(dev), family);
   pthread_mutex_lock(&g_mu);
   int free_slot = -1;
   bool known = false;
   for (int i = 0; i < MAX_QUEUES; ++i) {
      if (g_queues[i].q == q) known = true;
      if (!g_queues[i].q && free_slot < 0) free_slot = i;
   }
   if (!known && free_slot >= 0) {
      g_queues[free_slot].q = q;
      g_queues[free_slot].dev = dev;
      g_queues[free_slot].family = family;
   }
   for (int i = 0; i < MAX_DEVICES; ++i)
      if (g_devs[i].dev == dev && !g_devs[i].queue && graphics)
         g_devs[i].queue = q;
   pthread_mutex_unlock(&g_mu);
}

static void h_get_device_queue(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkDevice dev = (VkDevice)(uintptr_t)a->x[0];
   const uint32_t family = (uint32_t)a->x[1];
   VkQueue *out = (VkQueue *)(uintptr_t)a->x[3];
   PFN_vkGetDeviceQueue get = HOST(vkGetDeviceQueue);
   if (get && out) {
      get(dev, family, (uint32_t)a->x[2], out);
      note_queue(dev, family, *out);
   }
}

static void h_get_device_queue2(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkDevice dev = (VkDevice)(uintptr_t)a->x[0];
   const VkDeviceQueueInfo2 *info = (const VkDeviceQueueInfo2 *)(uintptr_t)a->x[1];
   VkQueue *out = (VkQueue *)(uintptr_t)a->x[2];
   PFN_vkGetDeviceQueue2 get = HOST(vkGetDeviceQueue2);
   if (get && info && out) {
      get(dev, info, out);
      note_queue(dev, info->queueFamilyIndex, *out);
   }
}

/* ---- memory -------------------------------------------------------------- */

/* Allocation sizes, for a map of VK_WHOLE_SIZE. */
static struct mem_size { VkDeviceMemory mem; VkDeviceSize size; } *g_mem;
static size_t g_nmem, g_capmem;

static void h_allocate_memory(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkDevice dev = (VkDevice)(uintptr_t)a->x[0];
   const VkMemoryAllocateInfo *info = (const VkMemoryAllocateInfo *)(uintptr_t)a->x[1];
   VkDeviceMemory *out = (VkDeviceMemory *)(uintptr_t)a->x[3];
   PFN_vkAllocateMemory alloc = HOST(vkAllocateMemory);
   const VkResult r = alloc ? alloc(dev, info, NULL, out) : VK_ERROR_INITIALIZATION_FAILED;
   if (r == VK_SUCCESS) {
      pthread_mutex_lock(&g_mu);
      if (g_nmem == g_capmem) {
         size_t cap = g_capmem ? g_capmem * 2 : 256;
         struct mem_size *nm = realloc(g_mem, cap * sizeof *nm);
         if (nm) { g_mem = nm; g_capmem = cap; }
      }
      if (g_nmem < g_capmem) {
         g_mem[g_nmem].mem = *out;
         g_mem[g_nmem].size = info->allocationSize;
         ++g_nmem;
      }
      pthread_mutex_unlock(&g_mu);
   }
   a->ret = vk_ret(r);
}

static void h_free_memory(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkDevice dev = (VkDevice)(uintptr_t)a->x[0];
   VkDeviceMemory mem = (VkDeviceMemory)(uintptr_t)a->x[1];
   PFN_vkFreeMemory release = HOST(vkFreeMemory);
   if (release) release(dev, mem, NULL);
   pthread_mutex_lock(&g_mu);
   for (size_t i = 0; i < g_nmem; ++i)
      if (g_mem[i].mem == mem) { g_mem[i] = g_mem[--g_nmem]; break; }
   pthread_mutex_unlock(&g_mu);
}

static VkDeviceSize mem_size_of(VkDeviceMemory mem)
{
   VkDeviceSize s = 0;
   pthread_mutex_lock(&g_mu);
   for (size_t i = 0; i < g_nmem; ++i)
      if (g_mem[i].mem == mem) { s = g_mem[i].size; break; }
   pthread_mutex_unlock(&g_mu);
   return s;
}

static void declare_mapping(VkDeviceMemory mem, VkDeviceSize offset,
                            VkDeviceSize size, void *ptr)
{
   if (!ptr) return;
   if (size == VK_WHOLE_SIZE) {
      const VkDeviceSize total = mem_size_of(mem);
      size = total > offset ? total - offset : 0;
   }
   if (size) arm64_exec_declare_host_range((uint64_t)(uintptr_t)ptr, size);
}

static void h_map_memory(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkDevice dev = (VkDevice)(uintptr_t)a->x[0];
   VkDeviceMemory mem = (VkDeviceMemory)(uintptr_t)a->x[1];
   void **pp = (void **)(uintptr_t)a->x[5];
   PFN_vkMapMemory map = HOST(vkMapMemory);
   const VkResult r = map && pp ? map(dev, mem, a->x[2], a->x[3],
                                      (VkMemoryMapFlags)a->x[4], pp)
                                : VK_ERROR_MEMORY_MAP_FAILED;
   if (r == VK_SUCCESS) declare_mapping(mem, a->x[2], a->x[3], *pp);
   a->ret = vk_ret(r);
}

static void h_map_memory2(HostCallArgs *a, uintptr_t idx)
{
   VkDevice dev = (VkDevice)(uintptr_t)a->x[0];
   const VkMemoryMapInfo *info = (const VkMemoryMapInfo *)(uintptr_t)a->x[1];
   void **pp = (void **)(uintptr_t)a->x[2];
   PFN_vkMapMemory2 map = (PFN_vkMapMemory2)host_fn_at((int)idx);
   const VkResult r = map && info && pp ? map(dev, info, pp)
                                        : VK_ERROR_MEMORY_MAP_FAILED;
   if (r == VK_SUCCESS)
      declare_mapping(info->memory, info->offset, info->size, *pp);
   a->ret = vk_ret(r);
}

/* ---- debug callbacks ----------------------------------------------------- */

static _Atomic(uint64_t) g_fake_handle = 0x10000;

static void h_create_debug_object(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   /* (instance, pCreateInfo, pAllocator, pHandle): the callback is guest
    * code; the handle is ours and nothing is ever reported through it. */
   uint64_t *out = (uint64_t *)(uintptr_t)a->x[3];
   if (out) *out = atomic_fetch_add(&g_fake_handle, 1);
   a->ret = vk_ret(VK_SUCCESS);
}

static void h_nothing(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   a->ret = 0;
}

/* ---- surfaces ------------------------------------------------------------ */

/* Any non-zero handle will do: nothing but this file ever sees a surface. */
static void h_create_android_surface(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkSurfaceKHR *out = (VkSurfaceKHR *)(uintptr_t)a->x[3];
   if (out) *out = (VkSurfaceKHR)(uintptr_t)atomic_fetch_add(&g_fake_handle, 1);
   a->ret = vk_ret(VK_SUCCESS);
}

static void frame_size(uint32_t *w, uint32_t *h)
{
   int fw = arm64_exec_fb_width(), fh = arm64_exec_fb_height();
   *w = fw > 0 ? (uint32_t)fw : 1280u;
   *h = fh > 0 ? (uint32_t)fh : 720u;
}

static void fill_caps(VkSurfaceCapabilitiesKHR *c)
{
   uint32_t w, h;
   frame_size(&w, &h);
   memset(c, 0, sizeof *c);
   c->minImageCount = 2;
   c->maxImageCount = 4;
   c->currentExtent.width = w;
   c->currentExtent.height = h;
   c->minImageExtent.width = 1;
   c->minImageExtent.height = 1;
   c->maxImageExtent.width = w > 4096u ? w : 4096u;
   c->maxImageExtent.height = h > 4096u ? h : 4096u;
   c->maxImageArrayLayers = 1;
   c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
                                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
   c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT;
}

static void h_surface_support(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkBool32 *out = (VkBool32 *)(uintptr_t)a->x[3];
   if (out)
      *out = family_has_graphics((VkPhysicalDevice)(uintptr_t)a->x[0],
                                 (uint32_t)a->x[1]) ? VK_TRUE : VK_FALSE;
   a->ret = vk_ret(VK_SUCCESS);
}

static void h_surface_caps(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkSurfaceCapabilitiesKHR *c = (VkSurfaceCapabilitiesKHR *)(uintptr_t)a->x[2];
   if (c) fill_caps(c);
   a->ret = vk_ret(VK_SUCCESS);
}

static void h_surface_caps2(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkSurfaceCapabilities2KHR *c = (VkSurfaceCapabilities2KHR *)(uintptr_t)a->x[2];
   if (c) fill_caps(&c->surfaceCapabilities);
   a->ret = vk_ret(VK_SUCCESS);
}

/* Four-byte formats only: the present path reads pixels back as RGBA. */
static const VkSurfaceFormatKHR k_formats[] = {
   { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
   { VK_FORMAT_R8G8B8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
};
#define NFORMATS (uint32_t)(sizeof k_formats / sizeof k_formats[0])

static void h_surface_formats(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   a->ret = vk_ret(fill_array((uint32_t *)(uintptr_t)a->x[2],
                              (void *)(uintptr_t)a->x[3], k_formats, NFORMATS,
                              sizeof k_formats[0]));
}

static void h_surface_formats2(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   uint32_t *count = (uint32_t *)(uintptr_t)a->x[2];
   VkSurfaceFormat2KHR *out = (VkSurfaceFormat2KHR *)(uintptr_t)a->x[3];
   if (!count) { a->ret = vk_ret(VK_ERROR_INITIALIZATION_FAILED); return; }
   if (!out) { *count = NFORMATS; a->ret = vk_ret(VK_SUCCESS); return; }
   const uint32_t k = *count < NFORMATS ? *count : NFORMATS;
   for (uint32_t i = 0; i < k; ++i) out[i].surfaceFormat = k_formats[i];
   *count = k;
   a->ret = vk_ret(k < NFORMATS ? VK_INCOMPLETE : VK_SUCCESS);
}

static const VkPresentModeKHR k_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
};

static void h_present_modes(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   a->ret = vk_ret(fill_array((uint32_t *)(uintptr_t)a->x[2],
                              (void *)(uintptr_t)a->x[3], k_modes,
                              (uint32_t)(sizeof k_modes / sizeof k_modes[0]),
                              sizeof k_modes[0]));
}

static void h_group_present_modes(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkDeviceGroupPresentModeFlagsKHR *out =
      (VkDeviceGroupPresentModeFlagsKHR *)(uintptr_t)a->x[2];
   if (out) *out = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
   a->ret = vk_ret(VK_SUCCESS);
}

static void h_present_rects(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkRect2D r = { { 0, 0 }, { 0, 0 } };
   frame_size(&r.extent.width, &r.extent.height);
   a->ret = vk_ret(fill_array((uint32_t *)(uintptr_t)a->x[2],
                              (void *)(uintptr_t)a->x[3], &r, 1, sizeof r));
}

/* ---- swapchains ------------------------------------------------------------ */

#define MAX_IMAGES 4

struct lvk_swapchain {
   VkDevice dev;
   VkFormat format;
   VkExtent2D extent;
   uint32_t count, next;
   bool fifo;
   VkImage img[MAX_IMAGES];
   VkDeviceMemory mem[MAX_IMAGES];
   VkBuffer buf;                       /* readback target               */
   VkDeviceMemory buf_mem;
   void *map;
   VkCommandPool pool;
   uint32_t pool_family;
   VkCommandBuffer cb[MAX_IMAGES];
   bool recorded[MAX_IMAGES];
   VkFence fence;
};

static int memory_type(VkPhysicalDevice pd, uint32_t bits,
                       VkMemoryPropertyFlags want)
{
   PFN_vkGetPhysicalDeviceMemoryProperties get =
      HOST(vkGetPhysicalDeviceMemoryProperties);
   if (!get) return -1;
   VkPhysicalDeviceMemoryProperties mp;
   get(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((bits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

static void swapchain_free(struct lvk_swapchain *sc)
{
   if (!sc) return;
   VkDevice d = sc->dev;
   PFN_vkDestroyImage di = HOST(vkDestroyImage);
   PFN_vkFreeMemory fm = HOST(vkFreeMemory);
   PFN_vkDestroyBuffer db = HOST(vkDestroyBuffer);
   PFN_vkUnmapMemory um = HOST(vkUnmapMemory);
   PFN_vkDestroyCommandPool dp = HOST(vkDestroyCommandPool);
   PFN_vkDestroyFence df = HOST(vkDestroyFence);
   if (sc->pool) dp(d, sc->pool, NULL);
   for (uint32_t i = 0; i < MAX_IMAGES; ++i) {
      if (sc->img[i]) di(d, sc->img[i], NULL);
      if (sc->mem[i]) fm(d, sc->mem[i], NULL);
   }
   if (sc->map) um(d, sc->buf_mem);
   if (sc->buf) db(d, sc->buf, NULL);
   if (sc->buf_mem) fm(d, sc->buf_mem, NULL);
   if (sc->fence) df(d, sc->fence, NULL);
   free(sc);
}

static VkResult swapchain_make(struct lvk_swapchain *sc,
                               const VkSwapchainCreateInfoKHR *ci)
{
   VkDevice d = sc->dev;
   VkPhysicalDevice pd = pd_of(d);
   PFN_vkCreateImage ci_ = HOST(vkCreateImage);
   PFN_vkGetImageMemoryRequirements imr = HOST(vkGetImageMemoryRequirements);
   PFN_vkAllocateMemory am = HOST(vkAllocateMemory);
   PFN_vkBindImageMemory bim = HOST(vkBindImageMemory);
   PFN_vkCreateBuffer cb = HOST(vkCreateBuffer);
   PFN_vkGetBufferMemoryRequirements bmr = HOST(vkGetBufferMemoryRequirements);
   PFN_vkBindBufferMemory bbm = HOST(vkBindBufferMemory);
   PFN_vkMapMemory mm = HOST(vkMapMemory);
   PFN_vkCreateFence cf = HOST(vkCreateFence);
   if (!pd || !ci_ || !imr || !am || !bim || !cb || !bmr || !bbm || !mm || !cf)
      return VK_ERROR_INITIALIZATION_FAILED;

   for (uint32_t i = 0; i < sc->count; ++i) {
      VkImageCreateInfo ic = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = sc->format,
         .extent = { sc->extent.width, sc->extent.height, 1 },
         .mipLevels = 1,
         .arrayLayers = ci->imageArrayLayers ? ci->imageArrayLayers : 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         .sharingMode = ci->imageSharingMode,
         .queueFamilyIndexCount = ci->queueFamilyIndexCount,
         .pQueueFamilyIndices = ci->pQueueFamilyIndices,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      VkResult r = ci_(d, &ic, NULL, &sc->img[i]);
      if (r != VK_SUCCESS) return r;
      VkMemoryRequirements req;
      imr(d, sc->img[i], &req);
      int type = memory_type(pd, req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (type < 0) type = memory_type(pd, req.memoryTypeBits, 0);
      VkMemoryAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = (uint32_t)type,
      };
      if (type < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      if ((r = am(d, &ai, NULL, &sc->mem[i])) != VK_SUCCESS) return r;
      if ((r = bim(d, sc->img[i], sc->mem[i], 0)) != VK_SUCCESS) return r;
   }

   VkBufferCreateInfo bc = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = (VkDeviceSize)sc->extent.width * sc->extent.height * 4u,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkResult r = cb(d, &bc, NULL, &sc->buf);
   if (r != VK_SUCCESS) return r;
   VkMemoryRequirements req;
   bmr(d, sc->buf, &req);
   const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   int type = memory_type(pd, req.memoryTypeBits,
                          host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
   if (type < 0) type = memory_type(pd, req.memoryTypeBits, host);
   if (type < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = (uint32_t)type,
   };
   if ((r = am(d, &ai, NULL, &sc->buf_mem)) != VK_SUCCESS) return r;
   if ((r = bbm(d, sc->buf, sc->buf_mem, 0)) != VK_SUCCESS) return r;
   if ((r = mm(d, sc->buf_mem, 0, VK_WHOLE_SIZE, 0, &sc->map)) != VK_SUCCESS)
      return r;
   VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   return cf(d, &fc, NULL, &sc->fence);
}

static void h_create_swapchain(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkDevice dev = (VkDevice)(uintptr_t)a->x[0];
   const VkSwapchainCreateInfoKHR *ci =
      (const VkSwapchainCreateInfoKHR *)(uintptr_t)a->x[1];
   VkSwapchainKHR *out = (VkSwapchainKHR *)(uintptr_t)a->x[3];
   struct lvk_swapchain *sc = ci && out ? calloc(1, sizeof *sc) : NULL;
   if (!sc) {
      a->ret = vk_ret(VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   sc->dev = dev;
   sc->format = ci->imageFormat;
   sc->extent = ci->imageExtent;
   sc->count = ci->minImageCount < 2 ? 2
             : ci->minImageCount > MAX_IMAGES ? MAX_IMAGES : ci->minImageCount;
   sc->fifo = ci->presentMode == VK_PRESENT_MODE_FIFO_KHR ||
              ci->presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
   sc->pool_family = UINT32_MAX;
   const VkResult r = swapchain_make(sc, ci);
   fprintf(stderr, "[vulkan] swapchain %ux%u format=%d images=%u -> %d\n",
           sc->extent.width, sc->extent.height, (int)sc->format, sc->count,
           (int)r);
   if (r != VK_SUCCESS) {
      swapchain_free(sc);
      a->ret = vk_ret(r);
      return;
   }
   *out = (VkSwapchainKHR)(uintptr_t)sc;
   a->ret = vk_ret(VK_SUCCESS);
}

static void h_destroy_swapchain(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   swapchain_free((struct lvk_swapchain *)(uintptr_t)a->x[1]);
}

static void h_get_swapchain_images(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   struct lvk_swapchain *sc = (struct lvk_swapchain *)(uintptr_t)a->x[1];
   if (!sc) { a->ret = vk_ret(VK_ERROR_SURFACE_LOST_KHR); return; }
   a->ret = vk_ret(fill_array((uint32_t *)(uintptr_t)a->x[2],
                              (void *)(uintptr_t)a->x[3], sc->img, sc->count,
                              sizeof sc->img[0]));
}

/* The image is free at once (present waits for its copy), so an acquire
 * only has to signal what the application gave it to wait on. */
static VkResult acquire(VkDevice dev, struct lvk_swapchain *sc,
                        VkSemaphore sem, VkFence fence, uint32_t *index)
{
   if (!sc || !index) return VK_ERROR_SURFACE_LOST_KHR;
   *index = sc->next;
   sc->next = (sc->next + 1u) % sc->count;
   if (!sem && !fence) return VK_SUCCESS;
   VkQueue q = queue_of(dev);
   PFN_vkQueueSubmit submit = HOST(vkQueueSubmit);
   if (!q || !submit) return VK_ERROR_DEVICE_LOST;
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
   if (sem) {
      si.signalSemaphoreCount = 1;
      si.pSignalSemaphores = &sem;
   }
   pthread_mutex_lock(&g_queue_mu);
   const VkResult r = submit(q, 1, &si, fence);
   pthread_mutex_unlock(&g_queue_mu);
   return r;
}

static void h_acquire_next_image(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   a->ret = vk_ret(acquire((VkDevice)(uintptr_t)a->x[0],
                           (struct lvk_swapchain *)(uintptr_t)a->x[1],
                           (VkSemaphore)(uintptr_t)a->x[3], (VkFence)(uintptr_t)a->x[4],
                           (uint32_t *)(uintptr_t)a->x[5]));
}

static void h_acquire_next_image2(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   const VkAcquireNextImageInfoKHR *info =
      (const VkAcquireNextImageInfoKHR *)(uintptr_t)a->x[1];
   if (!info) { a->ret = vk_ret(VK_ERROR_INITIALIZATION_FAILED); return; }
   a->ret = vk_ret(acquire((VkDevice)(uintptr_t)a->x[0],
                           (struct lvk_swapchain *)(uintptr_t)info->swapchain,
                           info->semaphore, info->fence,
                           (uint32_t *)(uintptr_t)a->x[2]));
}

/* The copy of one swapchain image into the readback buffer, recorded once
 * per image and resubmitted on every present. */
static VkResult readback_commands(struct lvk_swapchain *sc, uint32_t i,
                                  uint32_t family)
{
   VkDevice d = sc->dev;
   if (sc->pool && sc->pool_family != family) {
      HOST(vkDestroyCommandPool)(d, sc->pool, NULL);
      sc->pool = VK_NULL_HANDLE;
      memset(sc->recorded, 0, sizeof sc->recorded);
   }
   if (!sc->pool) {
      VkCommandPoolCreateInfo pc = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = family,
      };
      VkResult r = HOST(vkCreateCommandPool)(d, &pc, NULL, &sc->pool);
      if (r != VK_SUCCESS) return r;
      VkCommandBufferAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = sc->pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = sc->count,
      };
      if ((r = HOST(vkAllocateCommandBuffers)(d, &ai, sc->cb)) != VK_SUCCESS)
         return r;
      sc->pool_family = family;
   }
   if (sc->recorded[i]) return VK_SUCCESS;

   VkCommandBuffer cb = sc->cb[i];
   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   VkResult r = HOST(vkBeginCommandBuffer)(cb, &bi);
   if (r != VK_SUCCESS) return r;
   const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkImageMemoryBarrier to_src = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT |
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = sc->img[i],
      .subresourceRange = range,
   };
   HOST(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                              NULL, 1, &to_src);
   VkBufferImageCopy copy = {
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageExtent = { sc->extent.width, sc->extent.height, 1 },
   };
   HOST(vkCmdCopyImageToBuffer)(cb, sc->img[i],
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->buf,
                                1, &copy);
   VkImageMemoryBarrier back = to_src;
   back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   back.dstAccessMask = 0;
   back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   VkBufferMemoryBarrier to_host = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = sc->buf,
      .size = VK_WHOLE_SIZE,
   };
   HOST(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_HOST_BIT |
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              0, 0, NULL, 1, &to_host, 1, &back);
   if ((r = HOST(vkEndCommandBuffer)(cb)) != VK_SUCCESS) return r;
   sc->recorded[i] = true;
   return VK_SUCCESS;
}

static void h_queue_present(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   VkQueue q = (VkQueue)(uintptr_t)a->x[0];
   const VkPresentInfoKHR *pi = (const VkPresentInfoKHR *)(uintptr_t)a->x[1];
   if (!pi) { a->ret = vk_ret(VK_ERROR_INITIALIZATION_FAILED); return; }
   PFN_vkQueueSubmit submit = HOST(vkQueueSubmit);
   PFN_vkWaitForFences wait = HOST(vkWaitForFences);
   PFN_vkResetFences reset = HOST(vkResetFences);
   const uint32_t family = family_of(q);
   VkResult result = VK_SUCCESS;
   VkPipelineStageFlags stages[16];
   for (uint32_t k = 0; k < 16; ++k) stages[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

   for (uint32_t s = 0; s < pi->swapchainCount; ++s) {
      struct lvk_swapchain *sc = (struct lvk_swapchain *)(uintptr_t)pi->pSwapchains[s];
      const uint32_t i = pi->pImageIndices[s];
      VkResult r = sc && i < sc->count ? readback_commands(sc, i, family)
                                       : VK_ERROR_SURFACE_LOST_KHR;
      if (r == VK_SUCCESS) {
         VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &sc->cb[i],
         };
         if (s == 0 && pi->waitSemaphoreCount) {
            si.waitSemaphoreCount = pi->waitSemaphoreCount < 16
                                       ? pi->waitSemaphoreCount : 16;
            si.pWaitSemaphores = pi->pWaitSemaphores;
            si.pWaitDstStageMask = stages;
         }
         pthread_mutex_lock(&g_queue_mu);
         r = submit(q, 1, &si, sc->fence);
         pthread_mutex_unlock(&g_queue_mu);
      }
      if (r == VK_SUCCESS) {
         r = wait(sc->dev, 1, &sc->fence, VK_TRUE, 2000000000ull);
         reset(sc->dev, 1, &sc->fence);
      }
      if (r == VK_SUCCESS) {
         const bool bgra = sc->format == VK_FORMAT_B8G8R8A8_UNORM ||
                           sc->format == VK_FORMAT_B8G8R8A8_SRGB;
         luna_comp_submit_pixels((int)sc->extent.width, (int)sc->extent.height,
                                 sc->map, (int)sc->extent.width * 4, bgra,
                                 sc->fifo ? 1 : 0);
      }
      if (pi->pResults) pi->pResults[s] = r;
      if (r != VK_SUCCESS && result == VK_SUCCESS) result = r;
   }
   a->ret = vk_ret(result);
}

static void h_swapchain_status(HostCallArgs *a, uintptr_t idx)
{
   (void)idx;
   a->ret = vk_ret(VK_SUCCESS);
}

/* ---- setup ----------------------------------------------------------------- */

static const struct { const char *name; HostCallFn fn; } k_special[] = {
   { "vkGetInstanceProcAddr", h_get_instance_proc_addr },
   { "vkGetDeviceProcAddr", h_get_device_proc_addr },
   { "vkEnumerateInstanceExtensionProperties", h_enum_instance_ext },
   { "vkEnumerateInstanceLayerProperties", h_enum_instance_layers },
   { "vkEnumerateDeviceExtensionProperties", h_enum_device_ext },
   { "vkEnumerateDeviceLayerProperties", h_enum_device_layers },
   { "vkCreateInstance", h_create_instance },
   { "vkDestroyInstance", h_destroy_instance },
   { "vkCreateDevice", h_create_device },
   { "vkDestroyDevice", h_destroy_device },
   { "vkGetDeviceQueue", h_get_device_queue },
   { "vkGetDeviceQueue2", h_get_device_queue2 },
   { "vkAllocateMemory", h_allocate_memory },
   { "vkFreeMemory", h_free_memory },
   { "vkMapMemory", h_map_memory },
   { "vkMapMemory2", h_map_memory2 },
   { "vkMapMemory2KHR", h_map_memory2 },
   { "vkCreateDebugUtilsMessengerEXT", h_create_debug_object },
   { "vkDestroyDebugUtilsMessengerEXT", h_nothing },
   { "vkSubmitDebugUtilsMessageEXT", h_nothing },
   { "vkCreateDebugReportCallbackEXT", h_create_debug_object },
   { "vkDestroyDebugReportCallbackEXT", h_nothing },
   { "vkDebugReportMessageEXT", h_nothing },
   { "vkCreateAndroidSurfaceKHR", h_create_android_surface },
   { "vkDestroySurfaceKHR", h_nothing },
   { "vkGetPhysicalDeviceSurfaceSupportKHR", h_surface_support },
   { "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", h_surface_caps },
   { "vkGetPhysicalDeviceSurfaceCapabilities2KHR", h_surface_caps2 },
   { "vkGetPhysicalDeviceSurfaceFormatsKHR", h_surface_formats },
   { "vkGetPhysicalDeviceSurfaceFormats2KHR", h_surface_formats2 },
   { "vkGetPhysicalDeviceSurfacePresentModesKHR", h_present_modes },
   { "vkGetDeviceGroupSurfacePresentModesKHR", h_group_present_modes },
   { "vkGetPhysicalDevicePresentRectanglesKHR", h_present_rects },
   { "vkCreateSwapchainKHR", h_create_swapchain },
   { "vkDestroySwapchainKHR", h_destroy_swapchain },
   { "vkGetSwapchainImagesKHR", h_get_swapchain_images },
   { "vkAcquireNextImageKHR", h_acquire_next_image },
   { "vkAcquireNextImage2KHR", h_acquire_next_image2 },
   { "vkQueuePresentKHR", h_queue_present },
   { "vkGetSwapchainStatusKHR", h_swapchain_status },
};

/* Operations on a queue (or on every queue, vkDeviceWaitIdle). */
static const char *const k_queue_ops[] = {
   "vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR", "vkQueueWaitIdle",
   "vkQueueBindSparse", "vkDeviceWaitIdle", NULL,
};

static void vk_init(void)
{
   const char *e = getenv("LUNARIA_VULKAN");
   if (!e || e[0] != '1') return;
   if (!LVK_ABI_OK) {
      fprintf(stderr, "[vulkan] this host's calling convention is not "
              "supported; Vulkan stays off\n");
      return;
   }
   g_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!g_lib) g_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
   if (!g_lib) {
      fprintf(stderr, "[vulkan] no host libvulkan (%s); Vulkan stays off\n",
              dlerror());
      return;
   }
   g_gipa = (PFN_vkGetInstanceProcAddr)lib_fn("vkGetInstanceProcAddr");
   if (!g_gipa) {
      fprintf(stderr, "[vulkan] host libvulkan has no vkGetInstanceProcAddr\n");
      return;
   }
   for (uint32_t i = 0; i < NCMD; ++i) {
      const uint32_t h = fnv1a(lvk_cmds[i].name);
      for (uint32_t k = 0; k < HASH_SIZE; ++k) {
         uint16_t *slot = &g_hash[(h + k) & (HASH_SIZE - 1u)];
         if (!*slot) { *slot = (uint16_t)(i + 1u); break; }
      }
   }
   for (size_t i = 0; i < sizeof k_special / sizeof k_special[0]; ++i) {
      const int c = cmd_index(k_special[i].name);
      if (c >= 0) g_special[c] = k_special[i].fn;
      else fprintf(stderr, "[vulkan] %s is not in the command table\n",
                   k_special[i].name);
   }
   for (const char *const *q = k_queue_ops; *q; ++q) {
      const int c = cmd_index(*q);
      if (c >= 0) g_queue_op[c] = true;
   }
   g_ok = true;
   fprintf(stderr, "[vulkan] bridging to the host's libvulkan (%u commands)\n",
           (unsigned)NCMD);
}

uint64_t luna_vk_symbol(const char *name)
{
   pthread_once(&g_once, vk_init);
   if (!g_ok || !name || strncmp(name, "vk", 2) != 0) return 0;
   const int i = cmd_index(name);
   if (i < 0) return 0;
   /* dlsym() on libvulkan.so: what the host loader exports, plus the
    * Android entry points emulated here. */
   if (!g_special[i] && !dlsym(g_lib, name)) return 0;
   return tramp_at(i);
}
