#include "icd.h"

#include <stdlib.h>
#include <string.h>

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateFence(VkDevice device, const VkFenceCreateInfo *info,
                 const VkAllocationCallbacks *pAllocator, VkFence *pFence) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_fence_t *f = (cb_fence_t *)calloc(1, sizeof *f);
    if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
    f->device = dev;
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, info->flags);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_FENCE,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(f); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    f->remote_id = cb_r_u64(&r);
    free(reply);
    *pFence = (VkFence)CB_TO_HANDLE(f);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyFence(VkDevice device, VkFence fence,
                  const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!fence) return;
    cb_fence_t *f = CB_FROM_HANDLE(cb_fence_t, fence);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, f->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_FENCE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(f);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkWaitForFences(VkDevice device, uint32_t count, const VkFence *pFences,
                   VkBool32 waitAll, uint64_t timeout) {
    (void)device;
    cb_writer_t w; cb_writer_init_heap(&w, 32 + count * 8);
    cb_w_u32(&w, count);
    for (uint32_t i = 0; i < count; ++i) {
        cb_fence_t *f = CB_FROM_HANDLE(cb_fence_t, pFences[i]);
        cb_w_u64(&w, f ? f->remote_id : 0);
    }
    cb_w_u32(&w, waitAll);
    cb_w_u64(&w, timeout);
    VkResult vr = cb_rpc_call_void(CB_OP_WAIT_FOR_FENCES, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkResetFences(VkDevice device, uint32_t count, const VkFence *pFences) {
    (void)device;
    cb_writer_t w; cb_writer_init_heap(&w, 16 + count * 8);
    cb_w_u32(&w, count);
    for (uint32_t i = 0; i < count; ++i) {
        cb_fence_t *f = CB_FROM_HANDLE(cb_fence_t, pFences[i]);
        cb_w_u64(&w, f ? f->remote_id : 0);
    }
    VkResult vr = cb_rpc_call_void(CB_OP_RESET_FENCES, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkGetFenceStatus(VkDevice device, VkFence fence) {
    (void)device;
    cb_fence_t *f = CB_FROM_HANDLE(cb_fence_t, fence);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, f->remote_id);
    VkResult vr = cb_rpc_call_void(CB_OP_GET_FENCE_STATUS, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
                     const VkAllocationCallbacks *pAllocator,
                     VkSemaphore *pSem) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_semaphore_t *s = (cb_semaphore_t *)calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->device = dev;
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, info->flags);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_SEMAPHORE,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(s); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    s->remote_id = cb_r_u64(&r);
    free(reply);
    *pSem = (VkSemaphore)CB_TO_HANDLE(s);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroySemaphore(VkDevice device, VkSemaphore sem,
                      const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!sem) return;
    cb_semaphore_t *s = CB_FROM_HANDLE(cb_semaphore_t, sem);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, s->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_SEMAPHORE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(s);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateEvent(VkDevice device, const VkEventCreateInfo *info,
                 const VkAllocationCallbacks *pAllocator, VkEvent *pEvent) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_event_t *e = (cb_event_t *)calloc(1, sizeof *e);
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    e->device = dev;
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, info->flags);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_EVENT,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(e); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    e->remote_id = cb_r_u64(&r);
    free(reply);
    *pEvent = (VkEvent)CB_TO_HANDLE(e);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyEvent(VkDevice device, VkEvent event,
                  const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!event) return;
    cb_event_t *e = CB_FROM_HANDLE(cb_event_t, event);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, e->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_EVENT, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(e);
}
