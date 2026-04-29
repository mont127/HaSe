#include "icd.h"

#include <stdlib.h>
#include <string.h>

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                 const VkSubmitInfo *pSubmits, VkFence fence) {
    cb_queue_t *q = (cb_queue_t *)queue;
    cb_fence_t *f = fence ? CB_FROM_HANDLE(cb_fence_t, fence) : NULL;

    cb_writer_t w; cb_writer_init_heap(&w, 256);
    cb_w_u64(&w, q->remote_id);
    cb_w_u64(&w, f ? f->remote_id : 0);
    cb_w_u32(&w, submitCount);
    for (uint32_t i = 0; i < submitCount; ++i) {
        const VkSubmitInfo *s = &pSubmits[i];
        cb_w_u32(&w, s->waitSemaphoreCount);
        for (uint32_t j = 0; j < s->waitSemaphoreCount; ++j) {
            cb_semaphore_t *sm = CB_FROM_HANDLE(cb_semaphore_t,
                                                 s->pWaitSemaphores[j]);
            cb_w_u64(&w, sm ? sm->remote_id : 0);
            cb_w_u32(&w, s->pWaitDstStageMask[j]);
        }
        cb_w_u32(&w, s->commandBufferCount);
        for (uint32_t j = 0; j < s->commandBufferCount; ++j) {
            cb_command_buffer_t *cmd = (cb_command_buffer_t *)s->pCommandBuffers[j];
            cb_w_u64(&w, cmd ? cmd->remote_id : 0);
        }
        cb_w_u32(&w, s->signalSemaphoreCount);
        for (uint32_t j = 0; j < s->signalSemaphoreCount; ++j) {
            cb_semaphore_t *sm = CB_FROM_HANDLE(cb_semaphore_t,
                                                 s->pSignalSemaphores[j]);
            cb_w_u64(&w, sm ? sm->remote_id : 0);
        }
    }
    VkResult vr = cb_rpc_call_void(CB_OP_QUEUE_SUBMIT, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
    cb_queue_t *q = (cb_queue_t *)queue;
    cb_writer_t w; cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, q->remote_id);
    cb_w_u32(&w, info->waitSemaphoreCount);
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) {
        cb_semaphore_t *sm = CB_FROM_HANDLE(cb_semaphore_t,
                                             info->pWaitSemaphores[i]);
        cb_w_u64(&w, sm ? sm->remote_id : 0);
    }
    cb_w_u32(&w, info->swapchainCount);
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        cb_swapchain_t *sc = CB_FROM_HANDLE(cb_swapchain_t, info->pSwapchains[i]);
        cb_w_u64(&w, sc ? sc->remote_id : 0);
        cb_w_u32(&w, info->pImageIndices[i]);
    }
    VkResult vr = cb_rpc_call_void(CB_OP_QUEUE_PRESENT, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    if (vr == VK_SUCCESS && info->pResults) {
        for (uint32_t i = 0; i < info->swapchainCount; ++i)
            info->pResults[i] = VK_SUCCESS;
    }
    return vr;
}
