#include "icd.h"

#include <stdlib.h>
#include <string.h>

/* ---- Shader modules ------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *info,
                        const VkAllocationCallbacks *pAllocator,
                        VkShaderModule *pModule) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_shader_module_t *m = (cb_shader_module_t *)calloc(1, sizeof *m);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->device = dev;

    cb_writer_t w; cb_writer_init_heap(&w, info->codeSize + 32);
    cb_w_u64(&w, dev->remote_id);
    cb_w_blob(&w, info->pCode, info->codeSize);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_SHADER_MODULE,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(m); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    m->remote_id = cb_r_u64(&r);
    free(reply);
    *pModule = (VkShaderModule)CB_TO_HANDLE(m);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyShaderModule(VkDevice device, VkShaderModule module,
                         const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!module) return;
    cb_shader_module_t *m = CB_FROM_HANDLE(cb_shader_module_t, module);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, m->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_SHADER_MODULE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(m);
}

/* ---- Pipeline cache (opaque blob) --------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo *info,
                         const VkAllocationCallbacks *pAllocator,
                         VkPipelineCache *pCache) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_pipeline_cache_t *c = (cb_pipeline_cache_t *)calloc(1, sizeof *c);
    if (!c) return VK_ERROR_OUT_OF_HOST_MEMORY;
    c->device = dev;
    cb_writer_t w; cb_writer_init_heap(&w, info->initialDataSize + 32);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, info->flags);
    cb_w_blob(&w, info->pInitialData, info->initialDataSize);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_PIPELINE_CACHE,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(c); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    c->remote_id = cb_r_u64(&r);
    free(reply);
    *pCache = (VkPipelineCache)CB_TO_HANDLE(c);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyPipelineCache(VkDevice device, VkPipelineCache cache,
                          const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!cache) return;
    cb_pipeline_cache_t *c = CB_FROM_HANDLE(cb_pipeline_cache_t, cache);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, c->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_PIPELINE_CACHE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(c);
}

/* ---- Descriptor set layout ---------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateDescriptorSetLayout(VkDevice device,
                               const VkDescriptorSetLayoutCreateInfo *info,
                               const VkAllocationCallbacks *pAllocator,
                               VkDescriptorSetLayout *pLayout) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_descriptor_set_layout_t *l = (cb_descriptor_set_layout_t *)
        calloc(1, sizeof *l);
    if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
    l->device = dev;
    cb_writer_t w; cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, info->flags);
    cb_w_u32(&w, info->bindingCount);
    for (uint32_t i = 0; i < info->bindingCount; ++i) {
        const VkDescriptorSetLayoutBinding *b = &info->pBindings[i];
        cb_w_u32(&w, b->binding);
        cb_w_u32(&w, b->descriptorType);
        cb_w_u32(&w, b->descriptorCount);
        cb_w_u32(&w, b->stageFlags);
        /* immutable samplers: pass remote ids */
        cb_w_u32(&w, b->pImmutableSamplers ? b->descriptorCount : 0);
        if (b->pImmutableSamplers) {
            for (uint32_t j = 0; j < b->descriptorCount; ++j) {
                cb_sampler_t *s = CB_FROM_HANDLE(cb_sampler_t,
                                                 b->pImmutableSamplers[j]);
                cb_w_u64(&w, s ? s->remote_id : 0);
            }
        }
    }
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_DESC_SET_LAYOUT,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(l); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    l->remote_id = cb_r_u64(&r);
    free(reply);
    *pLayout = (VkDescriptorSetLayout)CB_TO_HANDLE(l);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout,
                                const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!layout) return;
    cb_descriptor_set_layout_t *l = CB_FROM_HANDLE(cb_descriptor_set_layout_t, layout);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, l->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_DESC_SET_LAYOUT, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(l);
}

/* ---- Descriptor pool / set ---------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateDescriptorPool(VkDevice device,
                          const VkDescriptorPoolCreateInfo *info,
                          const VkAllocationCallbacks *pAllocator,
                          VkDescriptorPool *pPool) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_descriptor_pool_t *p = (cb_descriptor_pool_t *)calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->device = dev;
    cb_writer_t w; cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, info->flags);
    cb_w_u32(&w, info->maxSets);
    cb_w_u32(&w, info->poolSizeCount);
    for (uint32_t i = 0; i < info->poolSizeCount; ++i) {
        cb_w_u32(&w, info->pPoolSizes[i].type);
        cb_w_u32(&w, info->pPoolSizes[i].descriptorCount);
    }
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_DESC_POOL,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(p); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    p->remote_id = cb_r_u64(&r);
    free(reply);
    *pPool = (VkDescriptorPool)CB_TO_HANDLE(p);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool,
                           const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!pool) return;
    cb_descriptor_pool_t *p = CB_FROM_HANDLE(cb_descriptor_pool_t, pool);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, p->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_DESC_POOL, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(p);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkAllocateDescriptorSets(VkDevice device,
                            const VkDescriptorSetAllocateInfo *info,
                            VkDescriptorSet *pSets) {
    cb_device_t *dev = (cb_device_t *)device;
    cb_descriptor_pool_t *pool = CB_FROM_HANDLE(cb_descriptor_pool_t, info->descriptorPool);

    cb_writer_t w; cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u64(&w, pool->remote_id);
    cb_w_u32(&w, info->descriptorSetCount);
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
        cb_descriptor_set_layout_t *l = CB_FROM_HANDLE(cb_descriptor_set_layout_t,
                                                        info->pSetLayouts[i]);
        cb_w_u64(&w, l->remote_id);
    }
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_ALLOCATE_DESC_SETS,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(reply); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
        cb_descriptor_set_t *ds = (cb_descriptor_set_t *)calloc(1, sizeof *ds);
        if (!ds) { free(reply); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        ds->device    = dev;
        ds->remote_id = cb_r_u64(&r);
        pSets[i] = (VkDescriptorSet)CB_TO_HANDLE(ds);
    }
    free(reply);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkFreeDescriptorSets(VkDevice device, VkDescriptorPool pool,
                        uint32_t count, const VkDescriptorSet *pSets) {
    cb_device_t *dev = (cb_device_t *)device;
    cb_descriptor_pool_t *p = CB_FROM_HANDLE(cb_descriptor_pool_t, pool);
    cb_writer_t w; cb_writer_init_heap(&w, 32 + count * sizeof(uint64_t));
    cb_w_u64(&w, dev->remote_id);
    cb_w_u64(&w, p->remote_id);
    cb_w_u32(&w, count);
    for (uint32_t i = 0; i < count; ++i) {
        cb_descriptor_set_t *ds = CB_FROM_HANDLE(cb_descriptor_set_t, pSets[i]);
        cb_w_u64(&w, ds ? ds->remote_id : 0);
    }
    VkResult vr = cb_rpc_call_void(CB_OP_FREE_DESC_SETS,
                                   w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    for (uint32_t i = 0; i < count; ++i)
        if (pSets[i]) free(CB_FROM_HANDLE(cb_descriptor_set_t, pSets[i]));
    return vr;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkUpdateDescriptorSets(VkDevice device,
                          uint32_t writeCount,
                          const VkWriteDescriptorSet *pWrites,
                          uint32_t copyCount,
                          const VkCopyDescriptorSet *pCopies) {
    cb_device_t *dev = (cb_device_t *)device;
    cb_writer_t w; cb_writer_init_heap(&w, 256);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, writeCount);
    for (uint32_t i = 0; i < writeCount; ++i) {
        const VkWriteDescriptorSet *wr = &pWrites[i];
        cb_descriptor_set_t *ds = CB_FROM_HANDLE(cb_descriptor_set_t, wr->dstSet);
        cb_w_u64(&w, ds->remote_id);
        cb_w_u32(&w, wr->dstBinding);
        cb_w_u32(&w, wr->dstArrayElement);
        cb_w_u32(&w, wr->descriptorCount);
        cb_w_u32(&w, wr->descriptorType);
        for (uint32_t j = 0; j < wr->descriptorCount; ++j) {
            switch (wr->descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                const VkDescriptorImageInfo *ii = &wr->pImageInfo[j];
                cb_sampler_t   *s  = ii->sampler   ? CB_FROM_HANDLE(cb_sampler_t,    ii->sampler)   : NULL;
                cb_image_view_t *iv = ii->imageView ? CB_FROM_HANDLE(cb_image_view_t, ii->imageView) : NULL;
                cb_w_u64(&w, s  ? s->remote_id  : 0);
                cb_w_u64(&w, iv ? iv->remote_id : 0);
                cb_w_u32(&w, ii->imageLayout);
            } break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                cb_buffer_view_t *bv = CB_FROM_HANDLE(cb_buffer_view_t,
                                                      wr->pTexelBufferView[j]);
                cb_w_u64(&w, bv ? bv->remote_id : 0);
            } break;
            default: {
                const VkDescriptorBufferInfo *bi = &wr->pBufferInfo[j];
                cb_buffer_t *bb = bi->buffer ? CB_FROM_HANDLE(cb_buffer_t, bi->buffer) : NULL;
                cb_w_u64(&w, bb ? bb->remote_id : 0);
                cb_w_u64(&w, bi->offset);
                cb_w_u64(&w, bi->range);
            } break;
            }
        }
    }
    cb_w_u32(&w, copyCount);
    for (uint32_t i = 0; i < copyCount; ++i) {
        const VkCopyDescriptorSet *c = &pCopies[i];
        cb_descriptor_set_t *src = CB_FROM_HANDLE(cb_descriptor_set_t, c->srcSet);
        cb_descriptor_set_t *dst = CB_FROM_HANDLE(cb_descriptor_set_t, c->dstSet);
        cb_w_u64(&w, src->remote_id);
        cb_w_u32(&w, c->srcBinding);
        cb_w_u32(&w, c->srcArrayElement);
        cb_w_u64(&w, dst->remote_id);
        cb_w_u32(&w, c->dstBinding);
        cb_w_u32(&w, c->dstArrayElement);
        cb_w_u32(&w, c->descriptorCount);
    }
    cb_rpc_send_async(CB_OP_UPDATE_DESC_SETS, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
}

/* ---- Pipeline layout ---------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreatePipelineLayout(VkDevice device,
                          const VkPipelineLayoutCreateInfo *info,
                          const VkAllocationCallbacks *pAllocator,
                          VkPipelineLayout *pLayout) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_pipeline_layout_t *l = (cb_pipeline_layout_t *)calloc(1, sizeof *l);
    if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
    l->device = dev;
    cb_writer_t w; cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, info->setLayoutCount);
    for (uint32_t i = 0; i < info->setLayoutCount; ++i) {
        cb_descriptor_set_layout_t *dsl =
            CB_FROM_HANDLE(cb_descriptor_set_layout_t, info->pSetLayouts[i]);
        cb_w_u64(&w, dsl ? dsl->remote_id : 0);
    }
    cb_w_u32(&w, info->pushConstantRangeCount);
    for (uint32_t i = 0; i < info->pushConstantRangeCount; ++i) {
        cb_w_u32(&w, info->pPushConstantRanges[i].stageFlags);
        cb_w_u32(&w, info->pPushConstantRanges[i].offset);
        cb_w_u32(&w, info->pPushConstantRanges[i].size);
    }
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_PIPELINE_LAYOUT,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(l); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    l->remote_id = cb_r_u64(&r);
    free(reply);
    *pLayout = (VkPipelineLayout)CB_TO_HANDLE(l);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout layout,
                           const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!layout) return;
    cb_pipeline_layout_t *l = CB_FROM_HANDLE(cb_pipeline_layout_t, layout);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, l->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_PIPELINE_LAYOUT, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(l);
}

/* ---- Render pass / framebuffer ----------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *info,
                      const VkAllocationCallbacks *pAllocator,
                      VkRenderPass *pRP) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_render_pass_t *rp = (cb_render_pass_t *)calloc(1, sizeof *rp);
    if (!rp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    rp->device = dev;

    cb_writer_t w; cb_writer_init_heap(&w, 256);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u32(&w, info->flags);
    cb_w_u32(&w, info->attachmentCount);
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const VkAttachmentDescription *a = &info->pAttachments[i];
        cb_w_u32(&w, a->flags);
        cb_w_u32(&w, a->format);
        cb_w_u32(&w, a->samples);
        cb_w_u32(&w, a->loadOp);
        cb_w_u32(&w, a->storeOp);
        cb_w_u32(&w, a->stencilLoadOp);
        cb_w_u32(&w, a->stencilStoreOp);
        cb_w_u32(&w, a->initialLayout);
        cb_w_u32(&w, a->finalLayout);
    }
    cb_w_u32(&w, info->subpassCount);
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        const VkSubpassDescription *sp = &info->pSubpasses[i];
        cb_w_u32(&w, sp->flags);
        cb_w_u32(&w, sp->pipelineBindPoint);
        cb_w_u32(&w, sp->inputAttachmentCount);
        for (uint32_t j = 0; j < sp->inputAttachmentCount; ++j) {
            cb_w_u32(&w, sp->pInputAttachments[j].attachment);
            cb_w_u32(&w, sp->pInputAttachments[j].layout);
        }
        cb_w_u32(&w, sp->colorAttachmentCount);
        for (uint32_t j = 0; j < sp->colorAttachmentCount; ++j) {
            cb_w_u32(&w, sp->pColorAttachments[j].attachment);
            cb_w_u32(&w, sp->pColorAttachments[j].layout);
            uint32_t resolve = sp->pResolveAttachments
                ? sp->pResolveAttachments[j].attachment : VK_ATTACHMENT_UNUSED;
            uint32_t rlay    = sp->pResolveAttachments
                ? sp->pResolveAttachments[j].layout : 0;
            cb_w_u32(&w, resolve);
            cb_w_u32(&w, rlay);
        }
        if (sp->pDepthStencilAttachment) {
            cb_w_u32(&w, sp->pDepthStencilAttachment->attachment);
            cb_w_u32(&w, sp->pDepthStencilAttachment->layout);
        } else {
            cb_w_u32(&w, VK_ATTACHMENT_UNUSED);
            cb_w_u32(&w, 0);
        }
        cb_w_u32(&w, sp->preserveAttachmentCount);
        for (uint32_t j = 0; j < sp->preserveAttachmentCount; ++j)
            cb_w_u32(&w, sp->pPreserveAttachments[j]);
    }
    cb_w_u32(&w, info->dependencyCount);
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        const VkSubpassDependency *d = &info->pDependencies[i];
        cb_w_u32(&w, d->srcSubpass);
        cb_w_u32(&w, d->dstSubpass);
        cb_w_u32(&w, d->srcStageMask);
        cb_w_u32(&w, d->dstStageMask);
        cb_w_u32(&w, d->srcAccessMask);
        cb_w_u32(&w, d->dstAccessMask);
        cb_w_u32(&w, d->dependencyFlags);
    }
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_RENDER_PASS,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(rp); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    rp->remote_id = cb_r_u64(&r);
    free(reply);
    *pRP = (VkRenderPass)CB_TO_HANDLE(rp);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyRenderPass(VkDevice device, VkRenderPass rp,
                       const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!rp) return;
    cb_render_pass_t *r = CB_FROM_HANDLE(cb_render_pass_t, rp);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, r->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_RENDER_PASS, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(r);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *info,
                       const VkAllocationCallbacks *pAllocator,
                       VkFramebuffer *pFB) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_framebuffer_t *fb = (cb_framebuffer_t *)calloc(1, sizeof *fb);
    if (!fb) return VK_ERROR_OUT_OF_HOST_MEMORY;
    fb->device = dev;
    cb_render_pass_t *rp = CB_FROM_HANDLE(cb_render_pass_t, info->renderPass);

    cb_writer_t w; cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u64(&w, rp ? rp->remote_id : 0);
    cb_w_u32(&w, info->flags);
    cb_w_u32(&w, info->attachmentCount);
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        cb_image_view_t *iv = CB_FROM_HANDLE(cb_image_view_t, info->pAttachments[i]);
        cb_w_u64(&w, iv ? iv->remote_id : 0);
    }
    cb_w_u32(&w, info->width);
    cb_w_u32(&w, info->height);
    cb_w_u32(&w, info->layers);
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_FRAMEBUFFER,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(fb); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    fb->remote_id = cb_r_u64(&r);
    free(reply);
    *pFB = (VkFramebuffer)CB_TO_HANDLE(fb);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyFramebuffer(VkDevice device, VkFramebuffer fb,
                        const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!fb) return;
    cb_framebuffer_t *f = CB_FROM_HANDLE(cb_framebuffer_t, fb);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, f->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_FRAMEBUFFER, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(f);
}

/* ---- Graphics / compute pipelines --------------------------------------- */

static void cb_serialize_shader_stage(cb_writer_t *w,
                                      const VkPipelineShaderStageCreateInfo *s) {
    cb_w_u32(w, s->flags);
    cb_w_u32(w, s->stage);
    cb_shader_module_t *m = CB_FROM_HANDLE(cb_shader_module_t, s->module);
    cb_w_u64(w, m ? m->remote_id : 0);
    const char *name = s->pName ? s->pName : "main";
    cb_w_blob(w, name, strlen(name));
    /* specialization constants */
    if (s->pSpecializationInfo) {
        const VkSpecializationInfo *si = s->pSpecializationInfo;
        cb_w_u32(w, si->mapEntryCount);
        for (uint32_t i = 0; i < si->mapEntryCount; ++i) {
            cb_w_u32(w, si->pMapEntries[i].constantID);
            cb_w_u32(w, si->pMapEntries[i].offset);
            cb_w_u32(w, (uint32_t)si->pMapEntries[i].size);
        }
        cb_w_blob(w, si->pData, si->dataSize);
    } else {
        cb_w_u32(w, 0);
        cb_w_blob(w, NULL, 0);
    }
}

static void cb_serialize_graphics_pipeline(cb_writer_t *w,
                                           const VkGraphicsPipelineCreateInfo *p) {
    cb_w_u32(w, p->flags);
    cb_w_u32(w, p->stageCount);
    for (uint32_t i = 0; i < p->stageCount; ++i)
        cb_serialize_shader_stage(w, &p->pStages[i]);

    /* Vertex input */
    const VkPipelineVertexInputStateCreateInfo *vi = p->pVertexInputState;
    cb_w_u32(w, vi ? vi->vertexBindingDescriptionCount : 0);
    if (vi) for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; ++i) {
        cb_w_u32(w, vi->pVertexBindingDescriptions[i].binding);
        cb_w_u32(w, vi->pVertexBindingDescriptions[i].stride);
        cb_w_u32(w, vi->pVertexBindingDescriptions[i].inputRate);
    }
    cb_w_u32(w, vi ? vi->vertexAttributeDescriptionCount : 0);
    if (vi) for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; ++i) {
        cb_w_u32(w, vi->pVertexAttributeDescriptions[i].location);
        cb_w_u32(w, vi->pVertexAttributeDescriptions[i].binding);
        cb_w_u32(w, vi->pVertexAttributeDescriptions[i].format);
        cb_w_u32(w, vi->pVertexAttributeDescriptions[i].offset);
    }

    /* Input assembly */
    cb_w_u32(w, p->pInputAssemblyState ? p->pInputAssemblyState->topology : 0);
    cb_w_u32(w, p->pInputAssemblyState ? p->pInputAssemblyState->primitiveRestartEnable : 0);

    /* Tessellation */
    cb_w_u32(w, p->pTessellationState ? p->pTessellationState->patchControlPoints : 0);

    /* Viewport (counts; values set dynamically in many DXVK pipelines) */
    cb_w_u32(w, p->pViewportState ? p->pViewportState->viewportCount : 0);
    cb_w_u32(w, p->pViewportState ? p->pViewportState->scissorCount  : 0);

    /* Rasterization */
    const VkPipelineRasterizationStateCreateInfo *r = p->pRasterizationState;
    cb_w_u32(w, r ? r->depthClampEnable        : 0);
    cb_w_u32(w, r ? r->rasterizerDiscardEnable : 0);
    cb_w_u32(w, r ? r->polygonMode             : 0);
    cb_w_u32(w, r ? r->cullMode                : 0);
    cb_w_u32(w, r ? r->frontFace               : 0);
    cb_w_u32(w, r ? r->depthBiasEnable         : 0);
    cb_w_f32(w, r ? r->depthBiasConstantFactor : 0);
    cb_w_f32(w, r ? r->depthBiasClamp          : 0);
    cb_w_f32(w, r ? r->depthBiasSlopeFactor    : 0);
    cb_w_f32(w, r ? r->lineWidth               : 1.0f);

    /* Multisample */
    const VkPipelineMultisampleStateCreateInfo *ms = p->pMultisampleState;
    cb_w_u32(w, ms ? ms->rasterizationSamples : 1);
    cb_w_u32(w, ms ? ms->sampleShadingEnable  : 0);
    cb_w_f32(w, ms ? ms->minSampleShading     : 0);
    cb_w_u32(w, ms ? ms->alphaToCoverageEnable: 0);
    cb_w_u32(w, ms ? ms->alphaToOneEnable     : 0);

    /* Depth-stencil */
    const VkPipelineDepthStencilStateCreateInfo *ds = p->pDepthStencilState;
    cb_w_u32(w, ds ? ds->depthTestEnable       : 0);
    cb_w_u32(w, ds ? ds->depthWriteEnable      : 0);
    cb_w_u32(w, ds ? ds->depthCompareOp        : 0);
    cb_w_u32(w, ds ? ds->depthBoundsTestEnable : 0);
    cb_w_u32(w, ds ? ds->stencilTestEnable     : 0);

    /* Color blend */
    const VkPipelineColorBlendStateCreateInfo *cb = p->pColorBlendState;
    cb_w_u32(w, cb ? cb->attachmentCount : 0);
    if (cb) for (uint32_t i = 0; i < cb->attachmentCount; ++i) {
        const VkPipelineColorBlendAttachmentState *a = &cb->pAttachments[i];
        cb_w_u32(w, a->blendEnable);
        cb_w_u32(w, a->srcColorBlendFactor);
        cb_w_u32(w, a->dstColorBlendFactor);
        cb_w_u32(w, a->colorBlendOp);
        cb_w_u32(w, a->srcAlphaBlendFactor);
        cb_w_u32(w, a->dstAlphaBlendFactor);
        cb_w_u32(w, a->alphaBlendOp);
        cb_w_u32(w, a->colorWriteMask);
    }

    /* Dynamic state */
    cb_w_u32(w, p->pDynamicState ? p->pDynamicState->dynamicStateCount : 0);
    if (p->pDynamicState)
        for (uint32_t i = 0; i < p->pDynamicState->dynamicStateCount; ++i)
            cb_w_u32(w, p->pDynamicState->pDynamicStates[i]);

    cb_pipeline_layout_t *pl = CB_FROM_HANDLE(cb_pipeline_layout_t, p->layout);
    cb_render_pass_t     *rp = CB_FROM_HANDLE(cb_render_pass_t,     p->renderPass);
    cb_w_u64(w, pl ? pl->remote_id : 0);
    cb_w_u64(w, rp ? rp->remote_id : 0);
    cb_w_u32(w, p->subpass);
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
                             uint32_t count,
                             const VkGraphicsPipelineCreateInfo *infos,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *pPipelines) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_pipeline_cache_t *c = cache ? CB_FROM_HANDLE(cb_pipeline_cache_t, cache) : NULL;

    cb_writer_t w; cb_writer_init_heap(&w, 1024);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u64(&w, c ? c->remote_id : 0);
    cb_w_u32(&w, count);
    for (uint32_t i = 0; i < count; ++i)
        cb_serialize_graphics_pipeline(&w, &infos[i]);

    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_GRAPHICS_PIPELINES,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(reply); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    for (uint32_t i = 0; i < count; ++i) {
        cb_pipeline_t *pp = (cb_pipeline_t *)calloc(1, sizeof *pp);
        if (!pp) { free(reply); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        pp->device    = dev;
        pp->remote_id = cb_r_u64(&r);
        pPipelines[i] = (VkPipeline)CB_TO_HANDLE(pp);
    }
    free(reply);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateComputePipelines(VkDevice device, VkPipelineCache cache,
                            uint32_t count,
                            const VkComputePipelineCreateInfo *infos,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipelines) {
    (void)pAllocator;
    cb_device_t *dev = (cb_device_t *)device;
    cb_pipeline_cache_t *c = cache ? CB_FROM_HANDLE(cb_pipeline_cache_t, cache) : NULL;

    cb_writer_t w; cb_writer_init_heap(&w, 256);
    cb_w_u64(&w, dev->remote_id);
    cb_w_u64(&w, c ? c->remote_id : 0);
    cb_w_u32(&w, count);
    for (uint32_t i = 0; i < count; ++i) {
        cb_w_u32(&w, infos[i].flags);
        cb_serialize_shader_stage(&w, &infos[i].stage);
        cb_pipeline_layout_t *pl = CB_FROM_HANDLE(cb_pipeline_layout_t, infos[i].layout);
        cb_w_u64(&w, pl ? pl->remote_id : 0);
    }
    void *reply = NULL; uint32_t rl = 0; uint16_t rop = 0;
    VkResult vr = cb_rpc_call(CB_OP_CREATE_COMPUTE_PIPELINES,
                              w.buf, (uint32_t)w.pos, &rop, &reply, &rl);
    cb_writer_dispose(&w);
    if (vr != VK_SUCCESS) { free(reply); return vr; }
    cb_reader_t r; cb_reader_init(&r, reply, rl);
    for (uint32_t i = 0; i < count; ++i) {
        cb_pipeline_t *pp = (cb_pipeline_t *)calloc(1, sizeof *pp);
        if (!pp) { free(reply); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        pp->device    = dev;
        pp->remote_id = cb_r_u64(&r);
        pPipelines[i] = (VkPipeline)CB_TO_HANDLE(pp);
    }
    free(reply);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
                     const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!pipeline) return;
    cb_pipeline_t *p = CB_FROM_HANDLE(cb_pipeline_t, pipeline);
    cb_writer_t w; cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, p->remote_id);
    cb_rpc_send_async(CB_OP_DESTROY_PIPELINE, w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    free(p);
}
