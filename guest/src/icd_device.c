#include "icd.h"

#include <stdlib.h>
#include <string.h>

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateDevice(VkPhysicalDevice physicalDevice,
                  const VkDeviceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkDevice *pDevice) {
    (void)pAllocator;
    cb_physical_device_t *pd = (cb_physical_device_t *)physicalDevice;
    if (!pd || !pCreateInfo || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;

    cb_device_t *dev = (cb_device_t *)cb_alloc_dispatchable(sizeof *dev);
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;
    dev->pd = pd;

    cb_writer_t w; cb_writer_init_heap(&w, 256);
    cb_w_u64(&w, pd->remote_id);
    cb_w_u32(&w, pCreateInfo->queueCreateInfoCount);
    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
        const VkDeviceQueueCreateInfo *q = &pCreateInfo->pQueueCreateInfos[i];
        cb_w_u32(&w, q->queueFamilyIndex);
        cb_w_u32(&w, q->queueCount);
        cb_w_u32(&w, q->flags);
        for (uint32_t j = 0; j < q->queueCount; ++j)
            cb_w_f32(&w, q->pQueuePriorities[j]);
    }
    cb_w_u32(&w, pCreateInfo->enabledExtensionCount);
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char *e = pCreateInfo->ppEnabledExtensionNames[i];
        cb_w_blob(&w, e, strlen(e));
    }
    /* Forward enabled features verbatim. */
    cb_w_opt_blob(&w, pCreateInfo->pEnabledFeatures,
                  pCreateInfo->pEnabledFeatures ? sizeof(VkPhysicalDeviceFeatures) : 0);

    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_DEVICE,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { cb_free_dispatchable(dev); return vr; }

    cb_reader_t r; cb_reader_init(&r, reply, rl);
    dev->remote_id = cb_r_u64(&r);
    free(reply);

    /* Pre-create cb_queue_t records lazily on vkGetDeviceQueue. */
    *pDevice = (VkDevice)dev;
    CB_I("vkCreateDevice -> remote_id=%llu", (unsigned long long)dev->remote_id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device) return;
    cb_device_t *dev = (cb_device_t *)device;

    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, dev->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_DEVICE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);

    if (dev->queues) {
        for (uint32_t i = 0; i < dev->queue_count; ++i)
            if (dev->queues[i]) cb_free_dispatchable(dev->queues[i]);
        free(dev->queues);
    }
    cb_free_dispatchable(dev);
}

VKAPI_ATTR void VKAPI_CALL
cb_vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index,
                    VkQueue *pQueue) {
    cb_device_t *dev = (cb_device_t *)device;
    if (!dev || !pQueue) return;

    /* Fetch (or reuse) the queue handle. */
    cb_writer_t w; cb_writer_init_heap(&w, 32);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, family);
    cb_w_u32(&w, index);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_GET_DEVICE_QUEUE,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { *pQueue = VK_NULL_HANDLE; return; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    cb_remote_id_t qid = cb_r_u64(&r);
    free(reply);

    /* Look up an existing wrapper or allocate a new one. */
    for (uint32_t i = 0; i < dev->queue_count; ++i) {
        if (dev->queues[i] && dev->queues[i]->remote_id == qid) {
            *pQueue = (VkQueue)dev->queues[i]; return;
        }
    }
    cb_queue_t *q = (cb_queue_t *)cb_alloc_dispatchable(sizeof *q);
    if (!q) { *pQueue = VK_NULL_HANDLE; return; }
    q->device    = dev;
    q->remote_id = qid;
    q->family    = family;
    q->index     = index;

    /* Append to dev->queues. */
    cb_queue_t **nq = (cb_queue_t **)realloc(dev->queues,
                          (dev->queue_count + 1) * sizeof *nq);
    if (!nq) { cb_free_dispatchable(q); *pQueue = VK_NULL_HANDLE; return; }
    dev->queues = nq;
    dev->queues[dev->queue_count++] = q;
    *pQueue = (VkQueue)q;
}

VKAPI_ATTR VkResult VKAPI_CALL cb_vkDeviceWaitIdle(VkDevice device) {
    cb_device_t *dev = (cb_device_t *)device;
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, dev->remote_id);
    VkResult vr = cb_rpc_call_void(CB_OP_DEVICE_WAIT_IDLE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL cb_vkQueueWaitIdle(VkQueue queue) {
    cb_queue_t *q = (cb_queue_t *)queue;
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, q->remote_id);
    VkResult vr = cb_rpc_call_void(CB_OP_QUEUE_WAIT_IDLE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}
