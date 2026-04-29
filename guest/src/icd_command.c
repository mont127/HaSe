#include "icd.h"

#include <stdlib.h>
#include <string.h>

/*
 * Command pool & command buffer recording.
 *
 * Recording is local: every vkCmd* call appends an entry to the command
 * buffer's `stream` cb_writer. The encoded format per entry is:
 *   uint16 op
 *   uint16 size      (total bytes including these 4)
 *   ... opcode-specific bytes ...
 *
 * vkEndCommandBuffer hands the recorded stream to the host via
 * CB_OP_RECORD_COMMAND_STREAM. The host re-replays each entry into a real
 * MoltenVK VkCommandBuffer that lives behind cmdbuf->remote_id.
 */

/* ---- Command pool ------------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateCommandPool(VkDevice device,
                       const VkCommandPoolCreateInfo *info,
                       const VkAllocationCallbacks *pAllocator,
                       VkCommandPool *pPool) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_command_pool_t *p = (cb_command_pool_t *)calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->device = dev;
    cb_writer_t w; cb_writer_init_heap(&w, 32);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, info->flags);
    cb_w_u32(&w, info->queueFamilyIndex);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_COMMAND_POOL,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(p); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    p->remote_id = cb_r_u64(&r);
    free(reply);
    *pPool = (VkCommandPool)CB_TO_HANDLE(p);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyCommandPool(VkDevice device, VkCommandPool pool,
                        const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!pool) return;
    cb_command_pool_t *p = CB_FROM_HANDLE(cb_command_pool_t, pool);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, p->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_COMMAND_POOL, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(p);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkResetCommandPool(VkDevice device, VkCommandPool pool,
                      VkCommandPoolResetFlags flags) {
    (void)device;
    cb_command_pool_t *p = CB_FROM_HANDLE(cb_command_pool_t, pool);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, p->remote_id);
    cb_w_u32(&w, flags);
    VkResult vr = cb_rpc_call_void(CB_OP_RESET_COMMAND_POOL,
                                   w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

/* ---- Command buffer alloc/free ----------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkAllocateCommandBuffers(VkDevice device,
                            const VkCommandBufferAllocateInfo *info,
                            VkCommandBuffer *pCBs) {
    cb_device_t *dev = (cb_device_t *)device;
    cb_command_pool_t *p = CB_FROM_HANDLE(cb_command_pool_t, info->commandPool);
    cb_writer_t w; cb_writer_init_heap(&w, 32);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u64(&w, p->remote_id);
    cb_w_u32(&w, info->level);
    cb_w_u32(&w, info->commandBufferCount);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_ALLOCATE_COMMAND_BUFFERS,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(reply); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        cb_command_buffer_t *cmd = (cb_command_buffer_t *)
            cb_alloc_dispatchable(sizeof *cmd);
        if (!cmd) { free(reply); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        cmd->device    = dev;
        cmd->pool_id   = p->remote_id;
        cmd->level     = info->level;
        cmd->remote_id = cb_r_u64(&r);
        cb_writer_init_heap(&cmd->stream, 1024);
        pCBs[i] = (VkCommandBuffer)cmd;
    }
    free(reply);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkFreeCommandBuffers(VkDevice device, VkCommandPool pool,
                        uint32_t count, const VkCommandBuffer *pCBs) {
    (void)device;
    cb_command_pool_t *p = CB_FROM_HANDLE(cb_command_pool_t, pool);
    cb_writer_t w; cb_writer_init_heap(&w, 32 + count * 8);
    cb_w_u64(&w, p->remote_id);
    cb_w_u32(&w, count);
    for (uint32_t i = 0; i < count; ++i) {
        cb_command_buffer_t *cmd = (cb_command_buffer_t *)pCBs[i];
        cb_w_u64(&w, cmd ? cmd->remote_id : 0);
    }
    cb_rpc_send_async(CB_OP_FREE_COMMAND_BUFFERS, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    for (uint32_t i = 0; i < count; ++i) {
        cb_command_buffer_t *cmd = (cb_command_buffer_t *)pCBs[i];
        if (!cmd) continue;
        cb_writer_dispose(&cmd->stream);
        cb_free_dispatchable(cmd);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkBeginCommandBuffer(VkCommandBuffer commandBuffer,
                        const VkCommandBufferBeginInfo *info) {
    cb_command_buffer_t *cmd = (cb_command_buffer_t *)commandBuffer;
    cmd->recording = true;
    cmd->stream.pos = 0;
    cmd->stream.overflow = false;

    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, cmd->remote_id);
    cb_w_u32(&w, info ? info->flags : 0);
    VkResult vr = cb_rpc_call_void(CB_OP_BEGIN_COMMAND_BUFFER,
                                   w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkEndCommandBuffer(VkCommandBuffer commandBuffer) {
    cb_command_buffer_t *cmd = (cb_command_buffer_t *)commandBuffer;
    cmd->recording = false;

    /* Ship the recorded stream. */
    cb_writer_t w; cb_writer_init_heap(&w, cmd->stream.pos + 32);
    cb_w_u64(&w, cmd->remote_id);
    cb_w_blob(&w, cmd->stream.buf, cmd->stream.pos);
    VkResult vr = cb_rpc_call_void(CB_OP_RECORD_COMMAND_STREAM,
                                   w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) return vr;

    cb_writer_t w2; cb_writer_init_heap(&w2, 16);
    cb_w_u64(&w2, cmd->remote_id);
    vr = cb_rpc_call_void(CB_OP_END_COMMAND_BUFFER,
                          w2.buf, (uint32_t)w2.pos);
    cb_writer_dispose(&w2);
    return vr;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkResetCommandBuffer(VkCommandBuffer commandBuffer,
                        VkCommandBufferResetFlags flags) {
    cb_command_buffer_t *cmd = (cb_command_buffer_t *)commandBuffer;
    cmd->stream.pos = 0;
    cmd->recording = false;
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, cmd->remote_id);
    cb_w_u32(&w, flags);
    VkResult vr = cb_rpc_call_void(CB_OP_RESET_COMMAND_BUFFER,
                                   w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return vr;
}

/* ---- vkCmd* recording helpers ------------------------------------------ */

/*
 * Begin/end an entry in the command stream. If the writer overflows the
 * caller's payload is silently truncated; vkQueueSubmit will surface the
 * problem when the host tries to decode it.
 */
static inline size_t cb_cmd_begin(cb_writer_t *w, uint16_t op) {
    cb_w_u16(w, op);
    cb_w_u16(w, 0);            /* size patched in cb_cmd_end */
    return w->pos - 2;
}
static inline void cb_cmd_end(cb_writer_t *w, size_t size_field_pos) {
    if (w->overflow || w->pos < size_field_pos + 2) return;
    uint16_t total = (uint16_t)(w->pos - (size_field_pos - 2));
    memcpy(w->buf + size_field_pos, &total, sizeof total);
}

#define CMD_BEGIN(cmd, op) \
    cb_writer_t *_w = &((cb_command_buffer_t *)(cmd))->stream; \
    size_t _sz = cb_cmd_begin(_w, (op))
#define CMD_END()  cb_cmd_end(_w, _sz)

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdBeginRenderPass(VkCommandBuffer cb,
                        const VkRenderPassBeginInfo *info,
                        VkSubpassContents contents) {
    CMD_BEGIN(cb, CB_CMD_BEGIN_RENDER_PASS);
    cb_render_pass_t *rp = CB_FROM_HANDLE(cb_render_pass_t, info->renderPass);
    cb_framebuffer_t *fb = CB_FROM_HANDLE(cb_framebuffer_t, info->framebuffer);
    cb_w_u64(_w, rp ? rp->remote_id : 0);
    cb_w_u64(_w, fb ? fb->remote_id : 0);
    cb_w_u32(_w, info->renderArea.offset.x);
    cb_w_u32(_w, info->renderArea.offset.y);
    cb_w_u32(_w, info->renderArea.extent.width);
    cb_w_u32(_w, info->renderArea.extent.height);
    cb_w_u32(_w, info->clearValueCount);
    for (uint32_t i = 0; i < info->clearValueCount; ++i)
        cb_w_bytes(_w, &info->pClearValues[i], sizeof(VkClearValue));
    cb_w_u32(_w, contents);
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL cb_vkCmdEndRenderPass(VkCommandBuffer cb) {
    CMD_BEGIN(cb, CB_CMD_END_RENDER_PASS);
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdNextSubpass(VkCommandBuffer cb, VkSubpassContents contents) {
    CMD_BEGIN(cb, CB_CMD_NEXT_SUBPASS);
    cb_w_u32(_w, contents);
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp,
                     VkPipeline pipeline) {
    CMD_BEGIN(cb, CB_CMD_BIND_PIPELINE);
    cb_pipeline_t *p = CB_FROM_HANDLE(cb_pipeline_t, pipeline);
    cb_w_u32(_w, bp);
    cb_w_u64(_w, p ? p->remote_id : 0);
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count,
                          const VkBuffer *pBuffers, const VkDeviceSize *pOffsets) {
    CMD_BEGIN(cb, CB_CMD_BIND_VERTEX_BUFFERS);
    cb_w_u32(_w, first);
    cb_w_u32(_w, count);
    for (uint32_t i = 0; i < count; ++i) {
        cb_buffer_t *b = pBuffers[i] ? CB_FROM_HANDLE(cb_buffer_t, pBuffers[i]) : NULL;
        cb_w_u64(_w, b ? b->remote_id : 0);
        cb_w_u64(_w, pOffsets[i]);
    }
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer,
                        VkDeviceSize offset, VkIndexType type) {
    CMD_BEGIN(cb, CB_CMD_BIND_INDEX_BUFFER);
    cb_buffer_t *b = buffer ? CB_FROM_HANDLE(cb_buffer_t, buffer) : NULL;
    cb_w_u64(_w, b ? b->remote_id : 0);
    cb_w_u64(_w, offset);
    cb_w_u32(_w, type);
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp,
                           VkPipelineLayout layout, uint32_t firstSet,
                           uint32_t setCount, const VkDescriptorSet *pSets,
                           uint32_t dynCount, const uint32_t *pDynOffsets) {
    CMD_BEGIN(cb, CB_CMD_BIND_DESCRIPTOR_SETS);
    cb_pipeline_layout_t *pl = CB_FROM_HANDLE(cb_pipeline_layout_t, layout);
    cb_w_u32(_w, bp);
    cb_w_u64(_w, pl ? pl->remote_id : 0);
    cb_w_u32(_w, firstSet);
    cb_w_u32(_w, setCount);
    for (uint32_t i = 0; i < setCount; ++i) {
        cb_descriptor_set_t *ds = CB_FROM_HANDLE(cb_descriptor_set_t, pSets[i]);
        cb_w_u64(_w, ds ? ds->remote_id : 0);
    }
    cb_w_u32(_w, dynCount);
    for (uint32_t i = 0; i < dynCount; ++i)
        cb_w_u32(_w, pDynOffsets[i]);
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout,
                      VkShaderStageFlags stages, uint32_t offset,
                      uint32_t size, const void *values) {
    CMD_BEGIN(cb, CB_CMD_PUSH_CONSTANTS);
    cb_pipeline_layout_t *pl = CB_FROM_HANDLE(cb_pipeline_layout_t, layout);
    cb_w_u64(_w, pl ? pl->remote_id : 0);
    cb_w_u32(_w, stages);
    cb_w_u32(_w, offset);
    cb_w_blob(_w, values, size);
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count,
                    const VkViewport *pViewports) {
    CMD_BEGIN(cb, CB_CMD_SET_VIEWPORT);
    cb_w_u32(_w, first);
    cb_w_u32(_w, count);
    cb_w_bytes(_w, pViewports, count * sizeof(VkViewport));
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count,
                   const VkRect2D *pScissors) {
    CMD_BEGIN(cb, CB_CMD_SET_SCISSOR);
    cb_w_u32(_w, first);
    cb_w_u32(_w, count);
    cb_w_bytes(_w, pScissors, count * sizeof(VkRect2D));
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL cb_vkCmdSetLineWidth(VkCommandBuffer cb, float width) {
    CMD_BEGIN(cb, CB_CMD_SET_LINE_WIDTH); cb_w_f32(_w, width); CMD_END();
}
VKAPI_ATTR void VKAPI_CALL
cb_vkCmdSetDepthBias(VkCommandBuffer cb, float c, float clamp, float slope) {
    CMD_BEGIN(cb, CB_CMD_SET_DEPTH_BIAS);
    cb_w_f32(_w, c); cb_w_f32(_w, clamp); cb_w_f32(_w, slope);
    CMD_END();
}
VKAPI_ATTR void VKAPI_CALL
cb_vkCmdSetBlendConstants(VkCommandBuffer cb, const float v[4]) {
    CMD_BEGIN(cb, CB_CMD_SET_BLEND_CONSTANTS);
    for (int i = 0; i < 4; ++i) cb_w_f32(_w, v[i]);
    CMD_END();
}
VKAPI_ATTR void VKAPI_CALL
cb_vkCmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
    CMD_BEGIN(cb, CB_CMD_SET_STENCIL_COMPARE_MASK);
    cb_w_u32(_w, f); cb_w_u32(_w, v); CMD_END();
}
VKAPI_ATTR void VKAPI_CALL
cb_vkCmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
    CMD_BEGIN(cb, CB_CMD_SET_STENCIL_WRITE_MASK);
    cb_w_u32(_w, f); cb_w_u32(_w, v); CMD_END();
}
VKAPI_ATTR void VKAPI_CALL
cb_vkCmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
    CMD_BEGIN(cb, CB_CMD_SET_STENCIL_REFERENCE);
    cb_w_u32(_w, f); cb_w_u32(_w, v); CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic,
             uint32_t fv, uint32_t fi) {
    CMD_BEGIN(cb, CB_CMD_DRAW);
    cb_w_u32(_w, vc); cb_w_u32(_w, ic); cb_w_u32(_w, fv); cb_w_u32(_w, fi);
    CMD_END();
}
VKAPI_ATTR void VKAPI_CALL
cb_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst,
                    uint32_t fi, int32_t vo, uint32_t fInst) {
    CMD_BEGIN(cb, CB_CMD_DRAW_INDEXED);
    cb_w_u32(_w, ic); cb_w_u32(_w, inst); cb_w_u32(_w, fi);
    cb_w_i32(_w, vo); cb_w_u32(_w, fInst);
    CMD_END();
}
VKAPI_ATTR void VKAPI_CALL
cb_vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize off,
                     uint32_t draw, uint32_t stride) {
    CMD_BEGIN(cb, CB_CMD_DRAW_INDIRECT);
    cb_buffer_t *b = CB_FROM_HANDLE(cb_buffer_t, buffer);
    cb_w_u64(_w, b ? b->remote_id : 0);
    cb_w_u64(_w, off); cb_w_u32(_w, draw); cb_w_u32(_w, stride);
    CMD_END();
}
VKAPI_ATTR void VKAPI_CALL
cb_vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buffer,
                            VkDeviceSize off, uint32_t draw, uint32_t stride) {
    CMD_BEGIN(cb, CB_CMD_DRAW_INDEXED_INDIRECT);
    cb_buffer_t *b = CB_FROM_HANDLE(cb_buffer_t, buffer);
    cb_w_u64(_w, b ? b->remote_id : 0);
    cb_w_u64(_w, off); cb_w_u32(_w, draw); cb_w_u32(_w, stride);
    CMD_END();
}
VKAPI_ATTR void VKAPI_CALL
cb_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    CMD_BEGIN(cb, CB_CMD_DISPATCH);
    cb_w_u32(_w, x); cb_w_u32(_w, y); cb_w_u32(_w, z);
    CMD_END();
}
VKAPI_ATTR void VKAPI_CALL
cb_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize off) {
    CMD_BEGIN(cb, CB_CMD_DISPATCH_INDIRECT);
    cb_buffer_t *b = CB_FROM_HANDLE(cb_buffer_t, buffer);
    cb_w_u64(_w, b ? b->remote_id : 0);
    cb_w_u64(_w, off);
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst,
                   uint32_t count, const VkBufferCopy *regions) {
    CMD_BEGIN(cb, CB_CMD_COPY_BUFFER);
    cb_buffer_t *bs = CB_FROM_HANDLE(cb_buffer_t, src);
    cb_buffer_t *bd = CB_FROM_HANDLE(cb_buffer_t, dst);
    cb_w_u64(_w, bs ? bs->remote_id : 0);
    cb_w_u64(_w, bd ? bd->remote_id : 0);
    cb_w_u32(_w, count);
    cb_w_bytes(_w, regions, count * sizeof(VkBufferCopy));
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdCopyImage(VkCommandBuffer cb, VkImage si, VkImageLayout sl,
                  VkImage di, VkImageLayout dl,
                  uint32_t count, const VkImageCopy *regions) {
    CMD_BEGIN(cb, CB_CMD_COPY_IMAGE);
    cb_image_t *is = CB_FROM_HANDLE(cb_image_t, si);
    cb_image_t *id_ = CB_FROM_HANDLE(cb_image_t, di);
    cb_w_u64(_w, is  ? is->remote_id  : 0); cb_w_u32(_w, sl);
    cb_w_u64(_w, id_ ? id_->remote_id : 0); cb_w_u32(_w, dl);
    cb_w_u32(_w, count);
    cb_w_bytes(_w, regions, count * sizeof(VkImageCopy));
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst,
                          VkImageLayout layout, uint32_t count,
                          const VkBufferImageCopy *regions) {
    CMD_BEGIN(cb, CB_CMD_COPY_BUFFER_TO_IMAGE);
    cb_buffer_t *bs = CB_FROM_HANDLE(cb_buffer_t, src);
    cb_image_t  *id_ = CB_FROM_HANDLE(cb_image_t, dst);
    cb_w_u64(_w, bs  ? bs->remote_id  : 0);
    cb_w_u64(_w, id_ ? id_->remote_id : 0);
    cb_w_u32(_w, layout);
    cb_w_u32(_w, count);
    cb_w_bytes(_w, regions, count * sizeof(VkBufferImageCopy));
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout,
                          VkBuffer dst, uint32_t count,
                          const VkBufferImageCopy *regions) {
    CMD_BEGIN(cb, CB_CMD_COPY_IMAGE_TO_BUFFER);
    cb_image_t  *is = CB_FROM_HANDLE(cb_image_t, src);
    cb_buffer_t *bd = CB_FROM_HANDLE(cb_buffer_t, dst);
    cb_w_u64(_w, is ? is->remote_id : 0); cb_w_u32(_w, layout);
    cb_w_u64(_w, bd ? bd->remote_id : 0);
    cb_w_u32(_w, count);
    cb_w_bytes(_w, regions, count * sizeof(VkBufferImageCopy));
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdBlitImage(VkCommandBuffer cb, VkImage si, VkImageLayout sl,
                  VkImage di, VkImageLayout dl, uint32_t count,
                  const VkImageBlit *regions, VkFilter filter) {
    CMD_BEGIN(cb, CB_CMD_BLIT_IMAGE);
    cb_image_t *is  = CB_FROM_HANDLE(cb_image_t, si);
    cb_image_t *id_ = CB_FROM_HANDLE(cb_image_t, di);
    cb_w_u64(_w, is  ? is->remote_id  : 0); cb_w_u32(_w, sl);
    cb_w_u64(_w, id_ ? id_->remote_id : 0); cb_w_u32(_w, dl);
    cb_w_u32(_w, count);
    cb_w_bytes(_w, regions, count * sizeof(VkImageBlit));
    cb_w_u32(_w, filter);
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src,
                        VkPipelineStageFlags dst, VkDependencyFlags dep,
                        uint32_t mb, const VkMemoryBarrier *pmb,
                        uint32_t bb, const VkBufferMemoryBarrier *pbb,
                        uint32_t ib, const VkImageMemoryBarrier *pib) {
    CMD_BEGIN(cb, CB_CMD_PIPELINE_BARRIER);
    cb_w_u32(_w, src); cb_w_u32(_w, dst); cb_w_u32(_w, dep);
    cb_w_u32(_w, mb);
    cb_w_bytes(_w, pmb, mb * sizeof(VkMemoryBarrier));
    cb_w_u32(_w, bb);
    for (uint32_t i = 0; i < bb; ++i) {
        cb_buffer_t *b = CB_FROM_HANDLE(cb_buffer_t, pbb[i].buffer);
        cb_w_u32(_w, pbb[i].srcAccessMask);
        cb_w_u32(_w, pbb[i].dstAccessMask);
        cb_w_u32(_w, pbb[i].srcQueueFamilyIndex);
        cb_w_u32(_w, pbb[i].dstQueueFamilyIndex);
        cb_w_u64(_w, b ? b->remote_id : 0);
        cb_w_u64(_w, pbb[i].offset);
        cb_w_u64(_w, pbb[i].size);
    }
    cb_w_u32(_w, ib);
    for (uint32_t i = 0; i < ib; ++i) {
        cb_image_t *im = CB_FROM_HANDLE(cb_image_t, pib[i].image);
        cb_w_u32(_w, pib[i].srcAccessMask);
        cb_w_u32(_w, pib[i].dstAccessMask);
        cb_w_u32(_w, pib[i].oldLayout);
        cb_w_u32(_w, pib[i].newLayout);
        cb_w_u32(_w, pib[i].srcQueueFamilyIndex);
        cb_w_u32(_w, pib[i].dstQueueFamilyIndex);
        cb_w_u64(_w, im ? im->remote_id : 0);
        cb_w_u32(_w, pib[i].subresourceRange.aspectMask);
        cb_w_u32(_w, pib[i].subresourceRange.baseMipLevel);
        cb_w_u32(_w, pib[i].subresourceRange.levelCount);
        cb_w_u32(_w, pib[i].subresourceRange.baseArrayLayer);
        cb_w_u32(_w, pib[i].subresourceRange.layerCount);
    }
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdClearColorImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                        const VkClearColorValue *color, uint32_t rangeCount,
                        const VkImageSubresourceRange *ranges) {
    CMD_BEGIN(cb, CB_CMD_CLEAR_COLOR_IMAGE);
    cb_image_t *im = CB_FROM_HANDLE(cb_image_t, image);
    cb_w_u64(_w, im ? im->remote_id : 0);
    cb_w_u32(_w, layout);
    cb_w_bytes(_w, color, sizeof(VkClearColorValue));
    cb_w_u32(_w, rangeCount);
    cb_w_bytes(_w, ranges, rangeCount * sizeof(VkImageSubresourceRange));
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t attCount,
                         const VkClearAttachment *atts,
                         uint32_t rectCount, const VkClearRect *rects) {
    CMD_BEGIN(cb, CB_CMD_CLEAR_ATTACHMENTS);
    cb_w_u32(_w, attCount);
    cb_w_bytes(_w, atts, attCount * sizeof(VkClearAttachment));
    cb_w_u32(_w, rectCount);
    cb_w_bytes(_w, rects, rectCount * sizeof(VkClearRect));
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset,
                   VkDeviceSize size, uint32_t data) {
    CMD_BEGIN(cb, CB_CMD_FILL_BUFFER);
    cb_buffer_t *b = CB_FROM_HANDLE(cb_buffer_t, dst);
    cb_w_u64(_w, b ? b->remote_id : 0);
    cb_w_u64(_w, offset); cb_w_u64(_w, size); cb_w_u32(_w, data);
    CMD_END();
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset,
                     VkDeviceSize size, const void *data) {
    CMD_BEGIN(cb, CB_CMD_UPDATE_BUFFER);
    cb_buffer_t *b = CB_FROM_HANDLE(cb_buffer_t, dst);
    cb_w_u64(_w, b ? b->remote_id : 0);
    cb_w_u64(_w, offset);
    cb_w_blob(_w, data, (size_t)size);
    CMD_END();
}
