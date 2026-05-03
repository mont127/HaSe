#include "icd.h"

#include <stdlib.h>
#include <string.h>

/*
 * The host owns the macOS NSWindow + CAMetalLayer. The guest sees a
 * VkSurfaceKHR whose remote_id corresponds to a window handle on the host
 * side. The guest exposes a CheeseBridge-specific extension entry point
 * (vkCreateCheeseBridgeSurfaceKHR) for the application to obtain that
 * surface; below, we wire up vkDestroySurfaceKHR and the surface queries.
 *
 * For DXVK / VKD3D-Proton scenarios the guest application typically calls
 * vkCreateXcbSurfaceKHR / vkCreateXlibSurfaceKHR. Those surface-creation
 * entry points should be added here once the host wraps an X11 surface
 * (out of scope for this layer; the host lives on macOS).
 */

/*
 * CheeseBridge-specific surface creation. The guest application calls this
 * directly (or through the loader's vkGetInstanceProcAddr) to get a
 * VkSurfaceKHR backed by an NSWindow + CAMetalLayer on the macOS host.
 *
 * Wire payload (CB_OP_CREATE_SURFACE):
 *   u64 instance_id | u32 width | u32 height
 * Reply: u64 surface_remote_id (host-side handle).
 */
VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateCheeseBridgeSurfaceKHR(VkInstance instance,
                                  uint32_t width, uint32_t height,
                                  VkSurfaceKHR *pSurface) {
    if (!instance || !pSurface) return VK_ERROR_INITIALIZATION_FAILED;
    cb_instance_t *inst = (cb_instance_t *)instance;

    cb_writer_t w; cb_writer_init_heap(&w, 24);
    cb_w_u64(&w, inst->remote_id);
    cb_w_u32(&w, width);
    cb_w_u32(&w, height);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_SURFACE,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(reply); return vr; }
    if (rl < sizeof(cb_remote_id_t)) { free(reply); return VK_ERROR_INITIALIZATION_FAILED; }

    cb_surface_t *s = (cb_surface_t *)calloc(1, sizeof *s);
    if (!s) { free(reply); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    memcpy(&s->remote_id, reply, sizeof s->remote_id);
    s->instance = inst;
    free(reply);
    *pSurface = (VkSurfaceKHR)CB_TO_HANDLE(s);
    return VK_SUCCESS;
}

static uint32_t cb_env_u32(const char *name, uint32_t fallback) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || parsed == 0 || parsed > UINT32_MAX) return fallback;
    return (uint32_t)parsed;
}

static uint32_t cb_surface_width(void) {
    uint32_t fallback = cb_env_u32("HASE_GAME_WIDTH", 1280);
    return cb_env_u32("CHEESEBRIDGE_SURFACE_WIDTH", fallback);
}

static uint32_t cb_surface_height(void) {
    uint32_t fallback = cb_env_u32("HASE_GAME_HEIGHT", 720);
    return cb_env_u32("CHEESEBRIDGE_SURFACE_HEIGHT", fallback);
}

/*
 * Steam/Proton/DXVK normally creates an Xlib or XCB Vulkan surface. The first
 * CheeseBridge game path maps those Linux WSI calls to a host-owned macOS
 * Metal window instead of trying to import the guest X11 window.
 */
VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateXlibSurfaceKHR(VkInstance instance,
                          const void *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkSurfaceKHR *pSurface) {
    (void)pCreateInfo;
    (void)pAllocator;
    uint32_t width = cb_surface_width();
    uint32_t height = cb_surface_height();
    CB_I("vkCreateXlibSurfaceKHR -> CheeseBridge host surface %ux%u",
         width, height);
    return cb_vkCreateCheeseBridgeSurfaceKHR(instance, width, height, pSurface);
}

VKAPI_ATTR VkBool32 VKAPI_CALL
cb_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                 uint32_t queueFamilyIndex,
                                                 void *dpy,
                                                 uintptr_t visualID) {
    (void)physicalDevice;
    (void)queueFamilyIndex;
    (void)dpy;
    (void)visualID;
    return VK_TRUE;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateXcbSurfaceKHR(VkInstance instance,
                         const void *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkSurfaceKHR *pSurface) {
    (void)pCreateInfo;
    (void)pAllocator;
    uint32_t width = cb_surface_width();
    uint32_t height = cb_surface_height();
    CB_I("vkCreateXcbSurfaceKHR -> CheeseBridge host surface %ux%u",
         width, height);
    return cb_vkCreateCheeseBridgeSurfaceKHR(instance, width, height, pSurface);
}

VKAPI_ATTR VkBool32 VKAPI_CALL
cb_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                uint32_t queueFamilyIndex,
                                                void *connection,
                                                uint32_t visual_id) {
    (void)physicalDevice;
    (void)queueFamilyIndex;
    (void)connection;
    (void)visual_id;
    return VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                       const VkAllocationCallbacks *pAllocator) {
    (void)instance; (void)pAllocator;
    if (!surface) return;
    cb_surface_t *s = CB_FROM_HANDLE(cb_surface_t, surface);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, s->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_SURFACE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(s);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
                                        uint32_t queueFamilyIndex,
                                        VkSurfaceKHR surface,
                                        VkBool32 *pSupported) {
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    cb_surface_t *s = CB_FROM_HANDLE(cb_surface_t, surface);
    cb_writer_t w; cb_writer_init_heap(&w, 32);
    cb_w_u64(&w, pd->remote_id);
    cb_w_u32(&w, queueFamilyIndex);
    cb_w_u64(&w, s->remote_id);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_GET_SURFACE_SUPPORT,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr == VK_SUCCESS && rl >= sizeof(uint32_t)) {
        uint32_t v; memcpy(&v, reply, sizeof v);
        *pSupported = v ? VK_TRUE : VK_FALSE;
    }
    free(reply);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                             VkSurfaceKHR surface,
                                             VkSurfaceCapabilitiesKHR *pCaps) {
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    cb_surface_t *s = CB_FROM_HANDLE(cb_surface_t, surface);
    cb_writer_t w; cb_writer_init_heap(&w, 24);
    cb_w_u64(&w, pd->remote_id);
    cb_w_u64(&w, s->remote_id);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_GET_SURFACE_CAPABILITIES,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr == VK_SUCCESS && rl >= sizeof(VkSurfaceCapabilitiesKHR))
        memcpy(pCaps, reply, sizeof *pCaps);
    free(reply);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
    VkSurfaceCapabilities2KHR *pSurfaceCapabilities) {
    if (!pSurfaceInfo || !pSurfaceCapabilities)
        return VK_ERROR_INITIALIZATION_FAILED;
    return cb_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice, pSurfaceInfo->surface,
        &pSurfaceCapabilities->surfaceCapabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
                                        VkSurfaceKHR surface,
                                        uint32_t *pCount,
                                        VkSurfaceFormatKHR *pFormats) {
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    cb_surface_t *s = CB_FROM_HANDLE(cb_surface_t, surface);
    cb_writer_t w; cb_writer_init_heap(&w, 24);
    cb_w_u64(&w, pd->remote_id);
    cb_w_u64(&w, s->remote_id);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_GET_SURFACE_FORMATS,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(reply); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    uint32_t n = cb_r_u32(&r);
    if (!pFormats) { *pCount = n; free(reply); return VK_SUCCESS; }
    uint32_t to_copy = (*pCount < n) ? *pCount : n;
    const void *bytes = cb_r_bytes(&r, n * sizeof(VkSurfaceFormatKHR));
    if (bytes && to_copy)
        memcpy(pFormats, bytes, to_copy * sizeof(VkSurfaceFormatKHR));
    *pCount = to_copy;
    free(reply);
    return (to_copy < n) ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
    uint32_t *pSurfaceFormatCount,
    VkSurfaceFormat2KHR *pSurfaceFormats) {
    if (!pSurfaceInfo || !pSurfaceFormatCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!pSurfaceFormats)
        return cb_vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice, pSurfaceInfo->surface, pSurfaceFormatCount, NULL);

    uint32_t requested = *pSurfaceFormatCount;
    VkSurfaceFormatKHR *tmp = requested
        ? (VkSurfaceFormatKHR *)calloc(requested, sizeof *tmp) : NULL;
    if (requested && !tmp) return VK_ERROR_OUT_OF_HOST_MEMORY;

    VkResult vr = cb_vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice, pSurfaceInfo->surface, pSurfaceFormatCount, tmp);
    uint32_t copied = *pSurfaceFormatCount;
    for (uint32_t i = 0; i < copied; ++i)
        pSurfaceFormats[i].surfaceFormat = tmp[i];
    free(tmp);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
                                             VkSurfaceKHR surface,
                                             uint32_t *pCount,
                                             VkPresentModeKHR *pModes) {
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    cb_surface_t *s = CB_FROM_HANDLE(cb_surface_t, surface);
    cb_writer_t w; cb_writer_init_heap(&w, 24);
    cb_w_u64(&w, pd->remote_id);
    cb_w_u64(&w, s->remote_id);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_GET_SURFACE_PRESENT_MODES,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(reply); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    uint32_t n = cb_r_u32(&r);
    if (!pModes) { *pCount = n; free(reply); return VK_SUCCESS; }
    uint32_t to_copy = (*pCount < n) ? *pCount : n;
    for (uint32_t i = 0; i < to_copy; ++i) pModes[i] = (VkPresentModeKHR)cb_r_u32(&r);
    *pCount = to_copy;
    free(reply);
    return (to_copy < n) ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetDeviceGroupPresentCapabilitiesKHR(
    VkDevice device,
    VkDeviceGroupPresentCapabilitiesKHR *pDeviceGroupPresentCapabilities) {
    (void)device;
    if (!pDeviceGroupPresentCapabilities)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < VK_MAX_DEVICE_GROUP_SIZE; ++i)
        pDeviceGroupPresentCapabilities->presentMask[i] = 0;
    pDeviceGroupPresentCapabilities->presentMask[0] = 1;
    pDeviceGroupPresentCapabilities->modes =
        VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetDeviceGroupSurfacePresentModesKHR(
    VkDevice device,
    VkSurfaceKHR surface,
    VkDeviceGroupPresentModeFlagsKHR *pModes) {
    (void)device;
    (void)surface;
    if (!pModes) return VK_ERROR_INITIALIZATION_FAILED;
    *pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice,
                                           VkSurfaceKHR surface,
                                           uint32_t *pRectCount,
                                           VkRect2D *pRects) {
    if (!pRectCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pRects) {
        *pRectCount = 1;
        return VK_SUCCESS;
    }

    VkSurfaceCapabilitiesKHR caps;
    VkResult vr = cb_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice, surface, &caps);
    if (vr != VK_SUCCESS) return vr;
    if (*pRectCount == 0) return VK_INCOMPLETE;
    pRects[0].offset.x = 0;
    pRects[0].offset.y = 0;
    pRects[0].extent = caps.currentExtent;
    if (pRects[0].extent.width == UINT32_MAX) {
        pRects[0].extent.width = caps.minImageExtent.width;
        pRects[0].extent.height = caps.minImageExtent.height;
    }
    *pRectCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateSwapchainKHR(VkDevice device,
                        const VkSwapchainCreateInfoKHR *info,
                        const VkAllocationCallbacks *pAllocator,
                        VkSwapchainKHR *pSwapchain) {
    (void)pAllocator;
    cb_device_t  *dev = (cb_device_t *)device;
    cb_surface_t *s   = CB_FROM_HANDLE(cb_surface_t, info->surface);
    cb_swapchain_t *sc = (cb_swapchain_t *)calloc(1, sizeof *sc);
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->device  = dev;
    sc->surface = s;
    sc->format  = info->imageFormat;
    sc->extent  = info->imageExtent;

    cb_writer_t w; cb_writer_init_heap(&w, 96);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u64(&w, s->remote_id);
    cb_w_u32(&w, info->flags);
    cb_w_u32(&w, info->minImageCount);
    cb_w_u32(&w, info->imageFormat);
    cb_w_u32(&w, info->imageColorSpace);
    cb_w_u32(&w, info->imageExtent.width);
    cb_w_u32(&w, info->imageExtent.height);
    cb_w_u32(&w, info->imageArrayLayers);
    cb_w_u32(&w, info->imageUsage);
    cb_w_u32(&w, info->imageSharingMode);
    cb_w_u32(&w, info->queueFamilyIndexCount);
    for (uint32_t i = 0; i < info->queueFamilyIndexCount; ++i)
        cb_w_u32(&w, info->pQueueFamilyIndices[i]);
    cb_w_u32(&w, info->preTransform);
    cb_w_u32(&w, info->compositeAlpha);
    cb_w_u32(&w, info->presentMode);
    cb_w_u32(&w, info->clipped);
    cb_w_u64(&w, info->oldSwapchain
        ? CB_FROM_HANDLE(cb_swapchain_t, info->oldSwapchain)->remote_id : 0);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_SWAPCHAIN,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(sc); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    sc->remote_id = cb_r_u64(&r);
    free(reply);
    *pSwapchain = (VkSwapchainKHR)CB_TO_HANDLE(sc);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                         const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!swapchain) return;
    cb_swapchain_t *sc = CB_FROM_HANDLE(cb_swapchain_t, swapchain);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, sc->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_SWAPCHAIN, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    if (sc->images) {
        for (uint32_t i = 0; i < sc->image_count; ++i)
            free(sc->images[i]);
        free(sc->images);
    }
    free(sc);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                           uint32_t *pCount, VkImage *pImages) {
    (void)device;
    cb_swapchain_t *sc = CB_FROM_HANDLE(cb_swapchain_t, swapchain);
    if (sc->image_count == 0) {
        cb_writer_t w; cb_writer_init_heap(&w, 16);
        cb_w_u64(&w, sc->remote_id);
        void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
        VkResult vr = cb_rpc_call(CB_OP_GET_SWAPCHAIN_IMAGES,
                                  w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
        cb_writer_dispose(&w);
        if (vr != VK_SUCCESS) { free(reply); return vr; }
        cb_reader_t r; cb_reader_init(&r, reply, rl);
        uint32_t n = cb_r_u32(&r);
        sc->images = n ? (cb_image_t **)calloc(n, sizeof *sc->images) : NULL;
        if (n && !sc->images) { free(reply); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        sc->image_count = n;
        for (uint32_t i = 0; i < n; ++i) {
            cb_image_t *img = (cb_image_t *)calloc(1, sizeof *img);
            if (!img) { free(reply); return VK_ERROR_OUT_OF_HOST_MEMORY; }
            img->device    = sc->device;
            img->remote_id = cb_r_u64(&r);
            sc->images[i]  = img;
        }
        free(reply);
    }
    if (!pImages) { *pCount = sc->image_count; return VK_SUCCESS; }
    uint32_t to_copy = (*pCount < sc->image_count) ? *pCount : sc->image_count;
    for (uint32_t i = 0; i < to_copy; ++i)
        pImages[i] = (VkImage)CB_TO_HANDLE(sc->images[i]);
    *pCount = to_copy;
    return (to_copy < sc->image_count) ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                         uint64_t timeout, VkSemaphore sem, VkFence fence,
                         uint32_t *pImageIndex) {
    (void)device;
    cb_swapchain_t *sc = CB_FROM_HANDLE(cb_swapchain_t, swapchain);
    cb_semaphore_t *s  = sem   ? CB_FROM_HANDLE(cb_semaphore_t, sem)   : NULL;
    cb_fence_t     *f  = fence ? CB_FROM_HANDLE(cb_fence_t,     fence) : NULL;
    cb_writer_t w; cb_writer_init_heap(&w, 40);
    cb_w_u64(&w, sc->remote_id);
    cb_w_u64(&w, timeout);
    cb_w_u64(&w, s ? s->remote_id : 0);
    cb_w_u64(&w, f ? f->remote_id : 0);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_ACQUIRE_NEXT_IMAGE,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        free(reply); return vr;
    }
    if (rl >= sizeof(uint32_t)) memcpy(pImageIndex, reply, sizeof(uint32_t));
    free(reply);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkAcquireNextImage2KHR(VkDevice device,
                          const VkAcquireNextImageInfoKHR *pAcquireInfo,
                          uint32_t *pImageIndex) {
    if (!pAcquireInfo) return VK_ERROR_INITIALIZATION_FAILED;
    return cb_vkAcquireNextImageKHR(device, pAcquireInfo->swapchain,
                                    pAcquireInfo->timeout,
                                    pAcquireInfo->semaphore,
                                    pAcquireInfo->fence,
                                    pImageIndex);
}
