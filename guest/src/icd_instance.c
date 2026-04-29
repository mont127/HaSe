#include "icd.h"

#include <stdlib.h>
#include <string.h>

/* ---- Instance-level discovery ------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
    if (pApiVersion) *pApiVersion = VK_API_VERSION_1_2;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkEnumerateInstanceLayerProperties(uint32_t *pCount, VkLayerProperties *pProps) {
    /* We expose no implicit layers. */
    if (pProps == NULL) { *pCount = 0; return VK_SUCCESS; }
    *pCount = 0;
    return VK_SUCCESS;
}

/* Surface-platform extensions advertised to the application.
 * The host owns the window; from the guest's perspective we only ever see
 * VK_KHR_surface + a guest-side shim extension that the application can use
 * to obtain a surface that maps to the host window. */
static const VkExtensionProperties g_inst_exts[] = {
    { VK_KHR_SURFACE_EXTENSION_NAME,                 25 },
    { "VK_CHEESEBRIDGE_host_surface",                 1 },
};

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                          VkExtensionProperties *pProps) {
    if (pLayerName) { *pCount = 0; return VK_SUCCESS; }
    uint32_t available = (uint32_t)(sizeof g_inst_exts / sizeof g_inst_exts[0]);
    if (!pProps) { *pCount = available; return VK_SUCCESS; }
    uint32_t to_copy = (*pCount < available) ? *pCount : available;
    memcpy(pProps, g_inst_exts, to_copy * sizeof g_inst_exts[0]);
    *pCount = to_copy;
    return (to_copy < available) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- vkCreateInstance --------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkInstance *pInstance) {
    (void)pAllocator;
    if (!pCreateInfo || !pInstance) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult vr = cb_transport_connect();
    if (vr != VK_SUCCESS) return vr;

    cb_instance_t *inst = (cb_instance_t *)cb_alloc_dispatchable(sizeof *inst);
    if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;

    inst->api_version = pCreateInfo->pApplicationInfo
        ? pCreateInfo->pApplicationInfo->apiVersion
        : VK_API_VERSION_1_0;

    cb_writer_t w; cb_writer_init_heap(&w, 128);
    cb_w_u32(&w, inst->api_version);
    const char *app =
        (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->pApplicationName)
        ? pCreateInfo->pApplicationInfo->pApplicationName : "";
    cb_w_blob(&w, app, strlen(app));

    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    vr = cb_rpc_call(CB_OP_CREATE_INSTANCE, w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { cb_free_dispatchable(inst); return vr; }

    cb_reader_t r; cb_reader_init(&r, reply, rl);
    inst->remote_id = cb_r_u64(&r);
    free(reply);

    *pInstance = (VkInstance)inst;
    CB_I("vkCreateInstance -> remote_id=%llu", (unsigned long long)inst->remote_id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!instance) return;
    cb_instance_t *inst = (cb_instance_t *)instance;

    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, inst->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_INSTANCE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);

    if (inst->pds) {
        for (uint32_t i = 0; i < inst->pd_count; ++i) {
            if (!inst->pds[i]) continue;
            free(inst->pds[i]->queue_families);
            cb_free_dispatchable(inst->pds[i]);
        }
        free(inst->pds);
    }
    cb_free_dispatchable(inst);
}

/* ---- Physical device enumeration ---------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pCount,
                              VkPhysicalDevice *pPDs) {
    cb_instance_t *inst = (cb_instance_t *)instance;
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;

    if (inst->pd_count == 0) {
        cb_writer_t w; cb_writer_init_heap(&w, 16);
        cb_w_u64(&w, inst->remote_id);

        void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
        VkResult vr = cb_rpc_call(CB_OP_ENUMERATE_PHYSICAL_DEVICES,
                                  w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
        cb_writer_dispose(&w);
        if (vr != VK_SUCCESS) return vr;

        cb_reader_t r; cb_reader_init(&r, reply, rl);
        uint32_t n = cb_r_u32(&r);
        inst->pds = n ? (cb_physical_device_t **)calloc(n, sizeof *inst->pds) : NULL;
        if (n && !inst->pds) { free(reply); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        inst->pd_count = n;
        for (uint32_t i = 0; i < n; ++i) {
            cb_physical_device_t *pd = (cb_physical_device_t *)cb_alloc_dispatchable(sizeof *pd);
            if (!pd) { free(reply); return VK_ERROR_OUT_OF_HOST_MEMORY; }
            pd->instance  = inst;
            pd->remote_id = cb_r_u64(&r);
            inst->pds[i]  = pd;
        }
        free(reply);
    }

    if (!pPDs) { *pCount = inst->pd_count; return VK_SUCCESS; }
    uint32_t to_copy = (*pCount < inst->pd_count) ? *pCount : inst->pd_count;
    for (uint32_t i = 0; i < to_copy; ++i) pPDs[i] = (VkPhysicalDevice)inst->pds[i];
    *pCount = to_copy;
    return (to_copy < inst->pd_count) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- Physical device queries -------------------------------------------- */

static VkResult cb_pd_query(cb_physical_device_t *pd, uint16_t op,
                            void **out_reply, uint32_t *out_len) {
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, pd->remote_id);
    uint16_t rop = 0;
    VkResult vr = cb_rpc_call(op, w.buf, (uint32_t)w.pos, &rop, out_reply, out_len);
    cb_writer_dispose(&w);
    return vr;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceProperties *pProps) {
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    if (!pd->props_cached) {
        void *reply = NULL; uint32_t rl = 0;
        if (cb_pd_query(pd, CB_OP_GET_PD_PROPERTIES, &reply, &rl) == VK_SUCCESS
            && rl >= sizeof(VkPhysicalDeviceProperties)) {
            memcpy(&pd->props, reply, sizeof pd->props);
            pd->props_cached = true;
        }
        free(reply);
    }
    if (pProps) *pProps = pd->props;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                               VkPhysicalDeviceFeatures *pFeatures) {
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    if (!pd->feats_cached) {
        void *reply = NULL; uint32_t rl = 0;
        if (cb_pd_query(pd, CB_OP_GET_PD_FEATURES, &reply, &rl) == VK_SUCCESS
            && rl >= sizeof(VkPhysicalDeviceFeatures)) {
            memcpy(&pd->feats, reply, sizeof pd->feats);
            pd->feats_cached = true;
        }
        free(reply);
    }
    if (pFeatures) *pFeatures = pd->feats;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                       VkPhysicalDeviceMemoryProperties *pMemProps) {
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    if (!pd->memprops_cached) {
        void *reply = NULL; uint32_t rl = 0;
        if (cb_pd_query(pd, CB_OP_GET_PD_MEMORY_PROPS, &reply, &rl) == VK_SUCCESS
            && rl >= sizeof(VkPhysicalDeviceMemoryProperties)) {
            memcpy(&pd->memprops, reply, sizeof pd->memprops);
            pd->memprops_cached = true;
        }
        free(reply);
    }
    if (pMemProps) *pMemProps = pd->memprops;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                            uint32_t *pCount,
                                            VkQueueFamilyProperties *pProps) {
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    if (!pd->queue_families) {
        void *reply = NULL; uint32_t rl = 0;
        if (cb_pd_query(pd, CB_OP_GET_PD_QUEUE_FAMILY_PROPS, &reply, &rl) == VK_SUCCESS) {
            cb_reader_t r; cb_reader_init(&r, reply, rl);
            uint32_t n = cb_r_u32(&r);
            const void *bytes = cb_r_bytes(&r, n * sizeof(VkQueueFamilyProperties));
            if (bytes) {
                pd->queue_families = (VkQueueFamilyProperties *)
                    malloc(n * sizeof(VkQueueFamilyProperties));
                if (pd->queue_families) {
                    memcpy(pd->queue_families, bytes,
                           n * sizeof(VkQueueFamilyProperties));
                    pd->queue_family_count = n;
                }
            }
        }
        free(reply);
    }
    if (!pProps) { *pCount = pd->queue_family_count; return; }
    uint32_t to_copy = (*pCount < pd->queue_family_count) ? *pCount : pd->queue_family_count;
    if (to_copy) memcpy(pProps, pd->queue_families,
                        to_copy * sizeof(VkQueueFamilyProperties));
    *pCount = to_copy;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                       VkFormat format,
                                       VkFormatProperties *pProps) {
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, pd->remote_id);
    cb_w_u32(&w, (uint32_t)format);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_GET_PD_FORMAT_PROPS,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr == VK_SUCCESS && rl >= sizeof(VkFormatProperties))
        memcpy(pProps, reply, sizeof(VkFormatProperties));
    else if (pProps)
        memset(pProps, 0, sizeof *pProps);
    free(reply);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice,
                                            VkFormat format, VkImageType type,
                                            VkImageTiling tiling,
                                            VkImageUsageFlags usage,
                                            VkImageCreateFlags flags,
                                            VkImageFormatProperties *pProps) {
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    cb_writer_t w; cb_writer_init_heap(&w, 32);
    cb_w_u64(&w, pd->remote_id);
    cb_w_u32(&w, (uint32_t)format);
    cb_w_u32(&w, (uint32_t)type);
    cb_w_u32(&w, (uint32_t)tiling);
    cb_w_u32(&w, (uint32_t)usage);
    cb_w_u32(&w, (uint32_t)flags);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_GET_PD_IMAGE_FORMAT_PROPS,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr == VK_SUCCESS && rl >= sizeof(VkImageFormatProperties))
        memcpy(pProps, reply, sizeof *pProps);
    free(reply);
    return vr;
}

/* Per-physical-device extension list. We re-use the host's. */
VKAPI_ATTR VkResult VKAPI_CALL
cb_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                        const char *pLayerName,
                                        uint32_t *pCount,
                                        VkExtensionProperties *pProps) {
    (void)physicalDevice; (void)pLayerName;
    /* Minimum useful set; the host might support more but we don't query
     * it dynamically here. Add to taste. */
    static const VkExtensionProperties exts[] = {
        { VK_KHR_SWAPCHAIN_EXTENSION_NAME, 70 },
        { VK_KHR_MAINTENANCE1_EXTENSION_NAME, 2 },
        { VK_KHR_MAINTENANCE2_EXTENSION_NAME, 1 },
        { VK_KHR_MAINTENANCE3_EXTENSION_NAME, 1 },
    };
    uint32_t available = (uint32_t)(sizeof exts / sizeof exts[0]);
    if (!pProps) { *pCount = available; return VK_SUCCESS; }
    uint32_t to_copy = (*pCount < available) ? *pCount : available;
    memcpy(pProps, exts, to_copy * sizeof exts[0]);
    *pCount = to_copy;
    return (to_copy < available) ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                    uint32_t *pCount, VkLayerProperties *pProps) {
    (void)physicalDevice;
    if (!pProps) { *pCount = 0; return VK_SUCCESS; }
    *pCount = 0;
    return VK_SUCCESS;
}
