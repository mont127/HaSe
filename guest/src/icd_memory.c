#include "icd.h"

#include <stdlib.h>
#include <string.h>

/*
 * Memory model:
 *   - The host owns the actual GPU-accessible allocation (Metal MTLBuffer
 *     or MTLHeap). We track a remote_id.
 *   - For HOST_VISIBLE allocations we additionally keep a guest-side
 *     mirror buffer so vkMapMemory can hand back a writable pointer.
 *     vkUnmapMemory / vkFlushMappedMemoryRanges ship the dirty range
 *     to the host with CB_OP_WRITE_MEMORY.
 *
 * The guest learns whether a memory type is HOST_VISIBLE by inspecting
 * the cached VkPhysicalDeviceMemoryProperties on the device's pd.
 */

static bool memtype_is_host_visible(cb_device_t *dev, uint32_t type_idx) {
    cb_physical_device_t *pd = dev->pd;
    if (!pd->memprops_cached) {
        VkPhysicalDeviceMemoryProperties tmp;
        cb_vkGetPhysicalDeviceMemoryProperties((VkPhysicalDevice)pd, &tmp);
    }
    if (type_idx >= pd->memprops.memoryTypeCount) return false;
    return (pd->memprops.memoryTypes[type_idx].propertyFlags
            & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                    const VkAllocationCallbacks *pAllocator,
                    VkDeviceMemory *pMemory) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_device_memory_t *m = (cb_device_memory_t *)calloc(1, sizeof *m);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;

    m->device            = dev;
    m->size              = info->allocationSize;
    m->memory_type_index = info->memoryTypeIndex;
    m->host_visible      = memtype_is_host_visible(dev, info->memoryTypeIndex);

    if (m->host_visible) {
        m->guest_mirror = malloc((size_t)info->allocationSize);
        if (!m->guest_mirror) { free(m); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    }

    cb_writer_t w; cb_writer_init_heap(&w, 32);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u64(&w, info->allocationSize);
    cb_w_u32(&w, info->memoryTypeIndex);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_ALLOCATE_MEMORY,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) {
        free(m->guest_mirror); free(m); return vr;
    }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    m->remote_id = cb_r_u64(&r);
    free(reply);
    *pMemory = (VkDeviceMemory)CB_TO_HANDLE(m);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!memory) return;
    cb_device_memory_t *m = CB_FROM_HANDLE(cb_device_memory_t, memory);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, m->remote_id);
    cb_rpc_send_async(CB_OP_FREE_MEMORY, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(m->guest_mirror);
    free(m);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
               VkDeviceSize size, VkMemoryMapFlags flags, void **ppData) {
    (void)device; (void)flags;
    cb_device_memory_t *m = CB_FROM_HANDLE(cb_device_memory_t, memory);
    if (!m->host_visible || !m->guest_mirror) return VK_ERROR_MEMORY_MAP_FAILED;
    if (size == VK_WHOLE_SIZE) size = m->size - offset;
    m->mapped_ptr    = (uint8_t *)m->guest_mirror + offset;
    m->mapped_offset = offset;
    m->mapped_size   = size;
    *ppData = m->mapped_ptr;

    /* Pull the current contents so reads through the map see real bytes. */
    cb_writer_t w; cb_writer_init_heap(&w, 32);
    cb_w_u64(&w, m->remote_id);
    cb_w_u64(&w, offset);
    cb_w_u64(&w, size);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_READ_MEMORY,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr == VK_SUCCESS && reply && rl >= size) {
        memcpy(m->mapped_ptr, reply, (size_t)size);
    }
    free(reply);
    return VK_SUCCESS;
}

static VkResult cb_push_range(cb_device_memory_t *m,
                              VkDeviceSize offset, VkDeviceSize size) {
    if (!m->host_visible || !m->guest_mirror) return VK_SUCCESS;
    if (size == VK_WHOLE_SIZE) size = m->size - offset;
    cb_writer_t w; cb_writer_init_heap(&w, (size_t)size + 32);
    cb_w_u64(&w, m->remote_id);
    cb_w_u64(&w, offset);
    cb_w_blob(&w, (uint8_t *)m->guest_mirror + offset, (size_t)size);
    VkResult vr = cb_rpc_call_void(CB_OP_WRITE_MEMORY, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    (void)device;
    cb_device_memory_t *m = CB_FROM_HANDLE(cb_device_memory_t, memory);
    if (!m->mapped_ptr) return;
    cb_push_range(m, m->mapped_offset, m->mapped_size);
    m->mapped_ptr = NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkFlushMappedMemoryRanges(VkDevice device, uint32_t count,
                             const VkMappedMemoryRange *ranges) {
    (void)device;
    for (uint32_t i = 0; i < count; ++i) {
        cb_device_memory_t *m = CB_FROM_HANDLE(cb_device_memory_t, ranges[i].memory);
        VkResult vr = cb_push_range(m, ranges[i].offset, ranges[i].size);
        if (vr != VK_SUCCESS) return vr;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t count,
                                  const VkMappedMemoryRange *ranges) {
    (void)device;
    /* Pull host -> guest mirror */
    for (uint32_t i = 0; i < count; ++i) {
        cb_device_memory_t *m = CB_FROM_HANDLE(cb_device_memory_t, ranges[i].memory);
        if (!m->host_visible || !m->guest_mirror) continue;
        VkDeviceSize size = ranges[i].size == VK_WHOLE_SIZE
            ? m->size - ranges[i].offset : ranges[i].size;
        cb_writer_t w; cb_writer_init_heap(&w, 32);
        cb_w_u64(&w, m->remote_id);
        cb_w_u64(&w, ranges[i].offset);
        cb_w_u64(&w, size);
        void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
        VkResult vr = cb_rpc_call(CB_OP_READ_MEMORY,
                                  w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
        cb_writer_dispose(&w);
        if (vr != VK_SUCCESS) { free(reply); return vr; }
        if (reply && rl >= size)
            memcpy((uint8_t *)m->guest_mirror + ranges[i].offset,
                   reply, (size_t)size);
        free(reply);
    }
    return VK_SUCCESS;
}

/* ---- Buffers ------------------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                  const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_buffer_t *b = (cb_buffer_t *)calloc(1, sizeof *b);
    if (!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    b->device = dev;

    cb_writer_t w; cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u64(&w, info->size);
    cb_w_u32(&w, info->usage);
    cb_w_u32(&w, info->flags);
    cb_w_u32(&w, info->sharingMode);
    cb_w_u32(&w, info->queueFamilyIndexCount);
    for (uint32_t i = 0; i < info->queueFamilyIndexCount; ++i)
        cb_w_u32(&w, info->pQueueFamilyIndices[i]);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_BUFFER,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(b); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    b->remote_id = cb_r_u64(&r);
    free(reply);
    *pBuffer = (VkBuffer)CB_TO_HANDLE(b);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyBuffer(VkDevice device, VkBuffer buffer,
                   const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!buffer) return;
    cb_buffer_t *b = CB_FROM_HANDLE(cb_buffer_t, buffer);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, b->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_BUFFER, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(b);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                 VkMemoryRequirements *pReq) {
    (void)device;
    cb_buffer_t *b = CB_FROM_HANDLE(cb_buffer_t, buffer);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, b->remote_id);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_GET_BUFFER_MEM_REQS,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr == VK_SUCCESS && rl >= sizeof(VkMemoryRequirements))
        memcpy(pReq, reply, sizeof *pReq);
    else if (pReq)
        memset(pReq, 0, sizeof *pReq);
    free(reply);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkBindBufferMemory(VkDevice device, VkBuffer buffer,
                      VkDeviceMemory memory, VkDeviceSize offset) {
    (void)device;
    cb_buffer_t        *b = CB_FROM_HANDLE(cb_buffer_t, buffer);
    cb_device_memory_t *m = CB_FROM_HANDLE(cb_device_memory_t, memory);
    cb_writer_t w; cb_writer_init_heap(&w, 32);
    cb_w_u64(&w, b->remote_id);
    cb_w_u64(&w, m->remote_id);
    cb_w_u64(&w, offset);
    VkResult vr = cb_rpc_call_void(CB_OP_BIND_BUFFER_MEMORY,
                                   w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetBufferMemoryRequirements2(VkDevice device,
                                  const VkBufferMemoryRequirementsInfo2 *pInfo,
                                  VkMemoryRequirements2 *pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    cb_vkGetBufferMemoryRequirements(device, pInfo->buffer,
                                     &pMemoryRequirements->memoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetBufferMemoryRequirements2KHR(VkDevice device,
                                     const VkBufferMemoryRequirementsInfo2 *pInfo,
                                     VkMemoryRequirements2 *pMemoryRequirements) {
    cb_vkGetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount,
                       const VkBindBufferMemoryInfo *pBindInfos) {
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        VkResult vr = cb_vkBindBufferMemory(device, pBindInfos[i].buffer,
                                            pBindInfos[i].memory,
                                            pBindInfos[i].memoryOffset);
        if (vr != VK_SUCCESS) return vr;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkBindBufferMemory2KHR(VkDevice device, uint32_t bindInfoCount,
                          const VkBindBufferMemoryInfo *pBindInfos) {
    return cb_vkBindBufferMemory2(device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo *info,
                      const VkAllocationCallbacks *pAllocator,
                      VkBufferView *pView) {
    (void)pAllocator;
    if (!device || !info || !pView) return VK_ERROR_INITIALIZATION_FAILED;
    cb_device_t *dev = (cb_device_t *)device;
    cb_buffer_view_t *bv = (cb_buffer_view_t *)calloc(1, sizeof *bv);
    if (!bv) return VK_ERROR_OUT_OF_HOST_MEMORY;
    bv->device = dev;
    bv->remote_id = cb_next_id();
    *pView = (VkBufferView)CB_TO_HANDLE(bv);
    CB_D("vkCreateBufferView placeholder id=%llu",
         (unsigned long long)bv->remote_id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyBufferView(VkDevice device, VkBufferView view,
                       const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    if (!view) return;
    free(CB_FROM_HANDLE(cb_buffer_view_t, view));
}

/* ---- Images -------------------------------------------------------------- */

static void cb_serialize_image_create(cb_writer_t *w,
                                      const VkImageCreateInfo *info) {
    cb_w_u32(w, info->flags);
    cb_w_u32(w, info->imageType);
    cb_w_u32(w, info->format);
    cb_w_u32(w, info->extent.width);
    cb_w_u32(w, info->extent.height);
    cb_w_u32(w, info->extent.depth);
    cb_w_u32(w, info->mipLevels);
    cb_w_u32(w, info->arrayLayers);
    cb_w_u32(w, info->samples);
    cb_w_u32(w, info->tiling);
    cb_w_u32(w, info->usage);
    cb_w_u32(w, info->sharingMode);
    cb_w_u32(w, info->queueFamilyIndexCount);
    for (uint32_t i = 0; i < info->queueFamilyIndexCount; ++i)
        cb_w_u32(w, info->pQueueFamilyIndices[i]);
    cb_w_u32(w, info->initialLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
                 const VkAllocationCallbacks *pAllocator, VkImage *pImage) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_image_t *img = (cb_image_t *)calloc(1, sizeof *img);
    if (!img) return VK_ERROR_OUT_OF_HOST_MEMORY;
    img->device = dev;

    cb_writer_t w; cb_writer_init_heap(&w, 128);
    cb_w_u64(&w, dev->remote_id);
    cb_serialize_image_create(&w, info);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_IMAGE,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(img); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    img->remote_id = cb_r_u64(&r);
    free(reply);
    *pImage = (VkImage)CB_TO_HANDLE(img);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyImage(VkDevice device, VkImage image,
                  const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!image) return;
    cb_image_t *img = CB_FROM_HANDLE(cb_image_t, image);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, img->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_IMAGE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(img);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                                VkMemoryRequirements *pReq) {
    (void)device;
    cb_image_t *img = CB_FROM_HANDLE(cb_image_t, image);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, img->remote_id);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_GET_IMAGE_MEM_REQS,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr == VK_SUCCESS && rl >= sizeof(VkMemoryRequirements))
        memcpy(pReq, reply, sizeof *pReq);
    else if (pReq)
        memset(pReq, 0, sizeof *pReq);
    free(reply);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkBindImageMemory(VkDevice device, VkImage image,
                     VkDeviceMemory memory, VkDeviceSize offset) {
    (void)device;
    cb_image_t         *img = CB_FROM_HANDLE(cb_image_t, image);
    cb_device_memory_t *m   = CB_FROM_HANDLE(cb_device_memory_t, memory);
    cb_writer_t w; cb_writer_init_heap(&w, 32);
    cb_w_u64(&w, img->remote_id);
    cb_w_u64(&w, m->remote_id);
    cb_w_u64(&w, offset);
    VkResult vr = cb_rpc_call_void(CB_OP_BIND_IMAGE_MEMORY,
                                   w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetImageMemoryRequirements2(VkDevice device,
                                 const VkImageMemoryRequirementsInfo2 *pInfo,
                                 VkMemoryRequirements2 *pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    cb_vkGetImageMemoryRequirements(device, pInfo->image,
                                    &pMemoryRequirements->memoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetImageMemoryRequirements2KHR(VkDevice device,
                                    const VkImageMemoryRequirementsInfo2 *pInfo,
                                    VkMemoryRequirements2 *pMemoryRequirements) {
    cb_vkGetImageMemoryRequirements2(device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetImageSparseMemoryRequirements(VkDevice device, VkImage image,
                                      uint32_t *pSparseMemoryRequirementCount,
                                      VkSparseImageMemoryRequirements *pSparseMemoryRequirements) {
    (void)device;
    (void)image;
    if (pSparseMemoryRequirementCount) *pSparseMemoryRequirementCount = 0;
    (void)pSparseMemoryRequirements;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetImageSparseMemoryRequirements2(
    VkDevice device,
    const VkImageSparseMemoryRequirementsInfo2 *pInfo,
    uint32_t *pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements) {
    (void)pInfo;
    if (pSparseMemoryRequirementCount) *pSparseMemoryRequirementCount = 0;
    (void)device;
    (void)pSparseMemoryRequirements;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetImageSparseMemoryRequirements2KHR(
    VkDevice device,
    const VkImageSparseMemoryRequirementsInfo2 *pInfo,
    uint32_t *pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements) {
    cb_vkGetImageSparseMemoryRequirements2(device, pInfo,
                                           pSparseMemoryRequirementCount,
                                           pSparseMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkBindImageMemory2(VkDevice device, uint32_t bindInfoCount,
                      const VkBindImageMemoryInfo *pBindInfos) {
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        VkResult vr = cb_vkBindImageMemory(device, pBindInfos[i].image,
                                           pBindInfos[i].memory,
                                           pBindInfos[i].memoryOffset);
        if (vr != VK_SUCCESS) return vr;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkBindImageMemory2KHR(VkDevice device, uint32_t bindInfoCount,
                         const VkBindImageMemoryInfo *pBindInfos) {
    return cb_vkBindImageMemory2(device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *info,
                     const VkAllocationCallbacks *pAllocator,
                     VkImageView *pView) {
    (void)pAllocator;
    cb_device_t  *dev = (cb_device_t *)device;
    cb_image_view_t *iv = (cb_image_view_t *)calloc(1, sizeof *iv);
    if (!iv) return VK_ERROR_OUT_OF_HOST_MEMORY;
    iv->device = dev;

    cb_image_t *img = CB_FROM_HANDLE(cb_image_t, info->image);

    cb_writer_t w; cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u64(&w, img->remote_id);
    cb_w_u32(&w, info->flags);
    cb_w_u32(&w, info->viewType);
    cb_w_u32(&w, info->format);
    cb_w_u32(&w, info->components.r);
    cb_w_u32(&w, info->components.g);
    cb_w_u32(&w, info->components.b);
    cb_w_u32(&w, info->components.a);
    cb_w_u32(&w, info->subresourceRange.aspectMask);
    cb_w_u32(&w, info->subresourceRange.baseMipLevel);
    cb_w_u32(&w, info->subresourceRange.levelCount);
    cb_w_u32(&w, info->subresourceRange.baseArrayLayer);
    cb_w_u32(&w, info->subresourceRange.layerCount);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_IMAGE_VIEW,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(iv); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    iv->remote_id = cb_r_u64(&r);
    free(reply);
    *pView = (VkImageView)CB_TO_HANDLE(iv);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyImageView(VkDevice device, VkImageView view,
                      const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!view) return;
    cb_image_view_t *iv = CB_FROM_HANDLE(cb_image_view_t, view);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, iv->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_IMAGE_VIEW, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(iv);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *info,
                   const VkAllocationCallbacks *pAllocator,
                   VkSampler *pSampler) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_sampler_t *s = (cb_sampler_t *)calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->device = dev;

    cb_writer_t w; cb_writer_init_heap(&w, 96);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, info->flags);
    cb_w_u32(&w, info->magFilter);
    cb_w_u32(&w, info->minFilter);
    cb_w_u32(&w, info->mipmapMode);
    cb_w_u32(&w, info->addressModeU);
    cb_w_u32(&w, info->addressModeV);
    cb_w_u32(&w, info->addressModeW);
    cb_w_f32(&w, info->mipLodBias);
    cb_w_u32(&w, info->anisotropyEnable);
    cb_w_f32(&w, info->maxAnisotropy);
    cb_w_u32(&w, info->compareEnable);
    cb_w_u32(&w, info->compareOp);
    cb_w_f32(&w, info->minLod);
    cb_w_f32(&w, info->maxLod);
    cb_w_u32(&w, info->borderColor);
    cb_w_u32(&w, info->unnormalizedCoordinates);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_SAMPLER,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(s); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    s->remote_id = cb_r_u64(&r);
    free(reply);
    *pSampler = (VkSampler)CB_TO_HANDLE(s);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroySampler(VkDevice device, VkSampler sampler,
                    const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!sampler) return;
    cb_sampler_t *s = CB_FROM_HANDLE(cb_sampler_t, sampler);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, s->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_SAMPLER, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(s);
}
