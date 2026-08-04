/*
 * libvulkan.so stub — the emulated Android device exposes GLES only.
 *
 * Vulkan loaders on Android are probed in two ways and both have to answer
 * consistently, otherwise the guest engine picks a RHI it cannot drive:
 *
 *   1. dlopen("libvulkan.so") + dlsym("vkGetInstanceProcAddr").  UE4's
 *      FVulkanDynamicRHIModule::IsSupported() and Unity's GfxDeviceVulkan
 *      both start here.  The library must therefore *exist* and export the
 *      loader entry points; a missing DT_NEEDED / dlopen failure makes the
 *      bionic linker abort the whole load instead of falling back.
 *   2. vkCreateInstance / vkEnumeratePhysicalDevices.  These must FAIL, so
 *      the engine drops the Vulkan RHI and selects OpenGL ES.
 *
 * Returning "success with an invalid handle" (what the generic unknown-symbol
 * stub does) is the one behaviour that breaks everything: the engine then
 * commits to Vulkan and dereferences a NULL dispatch table.  Every creation
 * and query entry point here fails explicitly, matching the SVC map in
 * arm_exec.cpp which routes the same names to SVC_RETM1 / SVC_RET0.
 */
#include <stddef.h>
#include <stdint.h>

#define VKAPI __attribute__((visibility("default")))

typedef int32_t VkResult;
/* VK_ERROR_INITIALIZATION_FAILED */
#define VK_ERROR_INIT_FAILED       (-3)
/* VK_ERROR_INCOMPATIBLE_DRIVER */
#define VK_ERROR_INCOMPATIBLE_DRV  (-9)
#define VK_SUCCESS                 0

typedef void (*PFN_vkVoidFunction)(void);

/* ---- loader entry points -------------------------------------------------
 * Returning NULL for every requested function is what a loader with no ICD
 * does.  Engines treat a NULL vkCreateInstance pointer as "no Vulkan". */
VKAPI PFN_vkVoidFunction vkGetInstanceProcAddr(void *instance, const char *name)
{
    (void)instance; (void)name;
    return NULL;
}

VKAPI PFN_vkVoidFunction vkGetDeviceProcAddr(void *device, const char *name)
{
    (void)device; (void)name;
    return NULL;
}

/* ---- instance / device creation: must fail ---- */
VKAPI VkResult vkCreateInstance(const void *ci, const void *alloc, void **out)
{
    (void)ci; (void)alloc;
    if (out) *out = NULL;
    return VK_ERROR_INCOMPATIBLE_DRV;
}

VKAPI void vkDestroyInstance(void *instance, const void *alloc)
{
    (void)instance; (void)alloc;
}

VKAPI VkResult vkCreateDevice(void *phys, const void *ci, const void *alloc, void **out)
{
    (void)phys; (void)ci; (void)alloc;
    if (out) *out = NULL;
    return VK_ERROR_INIT_FAILED;
}

VKAPI void vkDestroyDevice(void *device, const void *alloc)
{
    (void)device; (void)alloc;
}

/* ---- enumeration: report zero devices / zero extensions ----
 * vkEnumeratePhysicalDevices must write 0 into the count before returning:
 * callers that ignore the VkResult still read the count. */
VKAPI VkResult vkEnumeratePhysicalDevices(void *instance, uint32_t *count, void **devs)
{
    (void)instance; (void)devs;
    if (count) *count = 0;
    return VK_ERROR_INIT_FAILED;
}

VKAPI VkResult vkEnumerateInstanceExtensionProperties(const char *layer,
                                                      uint32_t *count, void *props)
{
    (void)layer; (void)props;
    if (count) *count = 0;
    return VK_SUCCESS;
}

VKAPI VkResult vkEnumerateInstanceLayerProperties(uint32_t *count, void *props)
{
    (void)props;
    if (count) *count = 0;
    return VK_SUCCESS;
}

VKAPI VkResult vkEnumerateDeviceExtensionProperties(void *phys, const char *layer,
                                                    uint32_t *count, void *props)
{
    (void)phys; (void)layer; (void)props;
    if (count) *count = 0;
    return VK_SUCCESS;
}

VKAPI VkResult vkEnumerateInstanceVersion(uint32_t *version)
{
    /* VK_API_VERSION_1_0 — the value a loader without an ICD still reports. */
    if (version) *version = (1u << 22);
    return VK_SUCCESS;
}

/* ---- physical-device queries: leave the caller's buffers untouched ---- */
VKAPI void vkGetPhysicalDeviceFeatures(void *phys, void *features)
{
    (void)phys; (void)features;
}

VKAPI void vkGetPhysicalDeviceProperties(void *phys, void *props)
{
    (void)phys; (void)props;
}

VKAPI void vkGetPhysicalDeviceMemoryProperties(void *phys, void *props)
{
    (void)phys; (void)props;
}

VKAPI void vkGetPhysicalDeviceQueueFamilyProperties(void *phys, uint32_t *count, void *props)
{
    (void)phys; (void)props;
    if (count) *count = 0;
}

/* ---- object creation reached only if the engine ignored the failures above.
 * Each fails so the first unchecked handle is never dereferenced. ---- */
#define VK_CREATE_STUB(name)                                              \
    VKAPI VkResult name(void *dev, const void *ci, const void *alloc, void **out) \
    { (void)dev; (void)ci; (void)alloc; if (out) *out = NULL;             \
      return VK_ERROR_INIT_FAILED; }

VK_CREATE_STUB(vkCreateShaderModule)
VK_CREATE_STUB(vkCreateRenderPass)
VK_CREATE_STUB(vkCreatePipelineCache)
VK_CREATE_STUB(vkCreateDescriptorSetLayout)
VK_CREATE_STUB(vkCreatePipelineLayout)

VKAPI VkResult vkCreateGraphicsPipelines(void *dev, void *cache, uint32_t n,
                                         const void *ci, const void *alloc, void **out)
{
    (void)dev; (void)cache; (void)ci; (void)alloc;
    if (out) for (uint32_t i = 0; i < n; ++i) out[i] = NULL;
    return VK_ERROR_INIT_FAILED;
}

VKAPI VkResult vkGetPipelineCacheData(void *dev, void *cache, size_t *size, void *data)
{
    (void)dev; (void)cache; (void)data;
    if (size) *size = 0;
    return VK_ERROR_INIT_FAILED;
}

#define VK_DESTROY_STUB(name)                                             \
    VKAPI void name(void *dev, void *obj, const void *alloc)              \
    { (void)dev; (void)obj; (void)alloc; }

VK_DESTROY_STUB(vkDestroyShaderModule)
VK_DESTROY_STUB(vkDestroyRenderPass)
VK_DESTROY_STUB(vkDestroyPipelineCache)
VK_DESTROY_STUB(vkDestroyPipeline)
VK_DESTROY_STUB(vkDestroyDescriptorSetLayout)
VK_DESTROY_STUB(vkDestroyPipelineLayout)
