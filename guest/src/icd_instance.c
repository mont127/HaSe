#include "icd.h"

#include <stdlib.h>
#include <string.h>

/* ---- Instance-level discovery ------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
    if (pApiVersion)
        *pApiVersion = cb_stub_mode_enabled() ? VK_API_VERSION_1_0
                                              : VK_API_VERSION_1_2;
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
    { "VK_KHR_xlib_surface",                          6 },
    { "VK_KHR_xcb_surface",                           6 },
    { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, 2 },
    { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,     1 },
    { VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,  1 },
    { VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,      1 },
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

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkImageType type,
    VkSampleCountFlagBits samples,
    VkImageUsageFlags usage,
    VkImageTiling tiling,
    uint32_t *pPropertyCount,
    VkSparseImageFormatProperties *pProperties) {
    (void)physicalDevice;
    (void)format;
    (void)type;
    (void)samples;
    (void)usage;
    (void)tiling;
    if (!pPropertyCount) return;
    *pPropertyCount = 0;
    (void)pProperties;
}

/* ---- VK_KHR_get_physical_device_properties2 ----------------------------- */

static void cb_fill_properties2_pnext(VkPhysicalDevice physicalDevice, void *pNext) {
    (void)physicalDevice;
    for (VkBaseOutStructure *out = (VkBaseOutStructure *)pNext; out; out = out->pNext) {
        switch (out->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
                VkPhysicalDeviceIDProperties *id = (VkPhysicalDeviceIDProperties *)out;
                memset(id->deviceUUID, 0x43, sizeof id->deviceUUID);
                memset(id->driverUUID, 0x42, sizeof id->driverUUID);
                memset(id->deviceLUID, 0, sizeof id->deviceLUID);
                id->deviceNodeMask = 0;
                id->deviceLUIDValid = VK_FALSE;
                break;
            }
            default:
                break;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceFeatures2 *pFeatures) {
    if (!pFeatures) return;
    cb_vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice,
                                   VkPhysicalDeviceFeatures2 *pFeatures) {
    cb_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceProperties2 *pProperties) {
    if (!pProperties) return;
    cb_vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);
    cb_fill_properties2_pnext(physicalDevice, pProperties->pNext);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice,
                                     VkPhysicalDeviceProperties2 *pProperties) {
    cb_vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                        VkFormat format,
                                        VkFormatProperties2 *pProperties) {
    if (!pProperties) return;
    cb_vkGetPhysicalDeviceFormatProperties(physicalDevice, format,
                                           &pProperties->formatProperties);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice physicalDevice,
                                           VkFormat format,
                                           VkFormatProperties2 *pProperties) {
    cb_vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, pProperties);
}

static const VkPhysicalDeviceExternalImageFormatInfo *
cb_find_external_image_format_info(const void *pNext) {
    for (const VkBaseInStructure *in = (const VkBaseInStructure *)pNext; in; in = in->pNext) {
        if (in->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO)
            return (const VkPhysicalDeviceExternalImageFormatInfo *)in;
    }
    return NULL;
}

static void cb_fill_external_image_format_properties(const void *pNext,
                                                    VkExternalMemoryHandleTypeFlagBits handleType) {
    for (VkBaseOutStructure *out = (VkBaseOutStructure *)pNext; out; out = out->pNext) {
        if (out->sType != VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES) continue;
        VkExternalImageFormatProperties *props = (VkExternalImageFormatProperties *)out;
        memset(&props->externalMemoryProperties, 0,
               sizeof props->externalMemoryProperties);
        props->externalMemoryProperties.compatibleHandleTypes = handleType;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
    VkImageFormatProperties2 *pImageFormatProperties) {
    if (!pImageFormatInfo || !pImageFormatProperties)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult vr = cb_vkGetPhysicalDeviceImageFormatProperties(
        physicalDevice, pImageFormatInfo->format, pImageFormatInfo->type,
        pImageFormatInfo->tiling, pImageFormatInfo->usage,
        pImageFormatInfo->flags, &pImageFormatProperties->imageFormatProperties);
    const VkPhysicalDeviceExternalImageFormatInfo *external =
        cb_find_external_image_format_info(pImageFormatInfo->pNext);
    if (external)
        cb_fill_external_image_format_properties(pImageFormatProperties->pNext,
                                                 external->handleType);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetPhysicalDeviceImageFormatProperties2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
    VkImageFormatProperties2 *pImageFormatProperties) {
    return cb_vkGetPhysicalDeviceImageFormatProperties2(
        physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                             uint32_t *pCount,
                                             VkQueueFamilyProperties2 *pProps) {
    if (!pProps) {
        cb_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pCount, NULL);
        return;
    }
    uint32_t count = pCount ? *pCount : 0;
    if (!count) return;
    VkQueueFamilyProperties *tmp =
        (VkQueueFamilyProperties *)calloc(count, sizeof *tmp);
    if (!tmp) {
        *pCount = 0;
        return;
    }
    cb_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, tmp);
    for (uint32_t i = 0; i < count; ++i) pProps[i].queueFamilyProperties = tmp[i];
    *pCount = count;
    free(tmp);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice physicalDevice,
                                                uint32_t *pCount,
                                                VkQueueFamilyProperties2 *pProps) {
    cb_vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, pCount, pProps);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                        VkPhysicalDeviceMemoryProperties2 *pMemProps) {
    if (!pMemProps) return;
    cb_vkGetPhysicalDeviceMemoryProperties(physicalDevice, &pMemProps->memoryProperties);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice physicalDevice,
                                           VkPhysicalDeviceMemoryProperties2 *pMemProps) {
    cb_vkGetPhysicalDeviceMemoryProperties2(physicalDevice, pMemProps);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
    uint32_t *pPropertyCount,
    VkSparseImageFormatProperties2 *pProperties) {
    (void)physicalDevice;
    (void)pFormatInfo;
    if (!pPropertyCount) return;
    *pPropertyCount = 0;
    (void)pProperties;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceSparseImageFormatProperties2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
    uint32_t *pPropertyCount,
    VkSparseImageFormatProperties2 *pProperties) {
    cb_vkGetPhysicalDeviceSparseImageFormatProperties2(
        physicalDevice, pFormatInfo, pPropertyCount, pProperties);
}

/* ---- External handle capability queries --------------------------------- */

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceExternalBufferProperties(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
    VkExternalBufferProperties *pExternalBufferProperties) {
    (void)physicalDevice;
    if (!pExternalBufferProperties) return;
    memset(&pExternalBufferProperties->externalMemoryProperties, 0,
           sizeof pExternalBufferProperties->externalMemoryProperties);
    if (pExternalBufferInfo)
        pExternalBufferProperties->externalMemoryProperties.compatibleHandleTypes =
            pExternalBufferInfo->handleType;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceExternalBufferPropertiesKHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
    VkExternalBufferProperties *pExternalBufferProperties) {
    cb_vkGetPhysicalDeviceExternalBufferProperties(
        physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceExternalFenceProperties(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo,
    VkExternalFenceProperties *pExternalFenceProperties) {
    (void)physicalDevice;
    if (!pExternalFenceProperties) return;
    pExternalFenceProperties->exportFromImportedHandleTypes = 0;
    pExternalFenceProperties->compatibleHandleTypes =
        pExternalFenceInfo ? pExternalFenceInfo->handleType : 0;
    pExternalFenceProperties->externalFenceFeatures = 0;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceExternalFencePropertiesKHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo,
    VkExternalFenceProperties *pExternalFenceProperties) {
    cb_vkGetPhysicalDeviceExternalFenceProperties(
        physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo,
    VkExternalSemaphoreProperties *pExternalSemaphoreProperties) {
    (void)physicalDevice;
    if (!pExternalSemaphoreProperties) return;
    pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
    pExternalSemaphoreProperties->compatibleHandleTypes =
        pExternalSemaphoreInfo ? pExternalSemaphoreInfo->handleType : 0;
    pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo,
    VkExternalSemaphoreProperties *pExternalSemaphoreProperties) {
    cb_vkGetPhysicalDeviceExternalSemaphoreProperties(
        physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
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
        { VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, 1 },
        { VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, 1 },
        { VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME, 1 },
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
