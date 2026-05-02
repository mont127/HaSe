#include "icd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool cb_env_false(const char *v) {
    return v && (!strcmp(v, "0") || !strcmp(v, "false") ||
                 !strcmp(v, "FALSE") || !strcmp(v, "off") ||
                 !strcmp(v, "OFF") || !strcmp(v, "no") ||
                 !strcmp(v, "NO"));
}

static bool cb_env_true(const char *v) {
    return v && (!strcmp(v, "1") || !strcmp(v, "true") ||
                 !strcmp(v, "TRUE") || !strcmp(v, "on") ||
                 !strcmp(v, "ON") || !strcmp(v, "yes") ||
                 !strcmp(v, "YES"));
}

bool cb_stub_mode_enabled(void) {
    static int cached = -1;
    if (cached >= 0) return cached != 0;

    const char *force = getenv("CHEESEBRIDGE_STUB");
    if (cb_env_true(force)) {
        cached = 1;
        return true;
    }
    if (cb_env_false(force)) {
        cached = 0;
        return false;
    }

    const char *host = getenv("CHEESEBRIDGE_HOST");
    cached = (!host || !*host) ? 1 : 0;
    return cached != 0;
}

static void cb_stub_write_reply(uint16_t opcode, cb_writer_t *w,
                                uint16_t *out_opcode, void **out_reply,
                                uint32_t *out_reply_len) {
    if (out_opcode) *out_opcode = opcode;
    if (out_reply_len) *out_reply_len = (uint32_t)w->pos;
    if (out_reply) {
        *out_reply = w->buf;
        w->buf = NULL;
        w->pos = 0;
        w->cap = 0;
    }
    cb_writer_dispose(w);
}

static VkResult cb_stub_empty_reply(uint16_t *out_opcode, void **out_reply,
                                    uint32_t *out_reply_len) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 1);
    cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                        out_reply_len);
    return VK_SUCCESS;
}

static VkResult cb_stub_id_reply(uint16_t *out_opcode, void **out_reply,
                                 uint32_t *out_reply_len) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 8);
    cb_w_u64(&w, cb_next_id());
    if (w.overflow) {
        cb_writer_dispose(&w);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                        out_reply_len);
    return VK_SUCCESS;
}

static void cb_stub_physical_device_properties(VkPhysicalDeviceProperties *p) {
    memset(p, 0, sizeof *p);
    p->apiVersion = VK_API_VERSION_1_0;
    p->driverVersion = VK_MAKE_VERSION(0, 2, 0);
    p->vendorID = 0x4342;
    p->deviceID = 0x0002;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    snprintf(p->deviceName, sizeof p->deviceName,
             "CheeseBridge Vulkan ICD Stub");
    memcpy(p->pipelineCacheUUID, "CheeseBridgeStub", VK_UUID_SIZE);

    p->limits.maxImageDimension1D = 4096;
    p->limits.maxImageDimension2D = 4096;
    p->limits.maxImageDimension3D = 256;
    p->limits.maxImageDimensionCube = 4096;
    p->limits.maxImageArrayLayers = 256;
    p->limits.maxTexelBufferElements = 65536;
    p->limits.maxUniformBufferRange = 65536;
    p->limits.maxStorageBufferRange = 128 * 1024 * 1024;
    p->limits.maxPushConstantsSize = 128;
    p->limits.maxMemoryAllocationCount = 4096;
    p->limits.maxSamplerAllocationCount = 4000;
    p->limits.bufferImageGranularity = 64;
    p->limits.maxBoundDescriptorSets = 4;
    p->limits.maxPerStageDescriptorSamplers = 16;
    p->limits.maxPerStageDescriptorUniformBuffers = 12;
    p->limits.maxPerStageDescriptorStorageBuffers = 4;
    p->limits.maxPerStageDescriptorSampledImages = 16;
    p->limits.maxPerStageDescriptorStorageImages = 4;
    p->limits.maxPerStageResources = 64;
    p->limits.maxDescriptorSetSamplers = 96;
    p->limits.maxDescriptorSetUniformBuffers = 72;
    p->limits.maxDescriptorSetStorageBuffers = 24;
    p->limits.maxDescriptorSetSampledImages = 96;
    p->limits.maxDescriptorSetStorageImages = 24;
    p->limits.maxVertexInputAttributes = 16;
    p->limits.maxVertexInputBindings = 16;
    p->limits.maxVertexInputAttributeOffset = 2047;
    p->limits.maxVertexInputBindingStride = 2048;
    p->limits.maxVertexOutputComponents = 64;
    p->limits.maxFragmentInputComponents = 64;
    p->limits.maxFragmentOutputAttachments = 4;
    p->limits.maxFragmentDualSrcAttachments = 1;
    p->limits.maxFragmentCombinedOutputResources = 4;
    p->limits.maxComputeSharedMemorySize = 16384;
    p->limits.maxComputeWorkGroupCount[0] = 65535;
    p->limits.maxComputeWorkGroupCount[1] = 65535;
    p->limits.maxComputeWorkGroupCount[2] = 65535;
    p->limits.maxComputeWorkGroupInvocations = 128;
    p->limits.maxComputeWorkGroupSize[0] = 128;
    p->limits.maxComputeWorkGroupSize[1] = 128;
    p->limits.maxComputeWorkGroupSize[2] = 64;
    p->limits.subPixelPrecisionBits = 4;
    p->limits.subTexelPrecisionBits = 4;
    p->limits.mipmapPrecisionBits = 4;
    p->limits.maxDrawIndexedIndexValue = 0x00ffffffu;
    p->limits.maxDrawIndirectCount = 1;
    p->limits.maxSamplerLodBias = 16.0f;
    p->limits.maxSamplerAnisotropy = 1.0f;
    p->limits.maxViewports = 1;
    p->limits.maxViewportDimensions[0] = 4096;
    p->limits.maxViewportDimensions[1] = 4096;
    p->limits.viewportBoundsRange[0] = -8192.0f;
    p->limits.viewportBoundsRange[1] = 8191.0f;
    p->limits.viewportSubPixelBits = 0;
    p->limits.minMemoryMapAlignment = 64;
    p->limits.minTexelBufferOffsetAlignment = 16;
    p->limits.minUniformBufferOffsetAlignment = 16;
    p->limits.minStorageBufferOffsetAlignment = 16;
    p->limits.minTexelOffset = -8;
    p->limits.maxTexelOffset = 7;
    p->limits.minTexelGatherOffset = -8;
    p->limits.maxTexelGatherOffset = 7;
    p->limits.minInterpolationOffset = 0.0f;
    p->limits.maxInterpolationOffset = 0.0f;
    p->limits.subPixelInterpolationOffsetBits = 0;
    p->limits.maxFramebufferWidth = 4096;
    p->limits.maxFramebufferHeight = 4096;
    p->limits.maxFramebufferLayers = 256;
    p->limits.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    p->limits.framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    p->limits.framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    p->limits.framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    p->limits.maxColorAttachments = 4;
    p->limits.sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    p->limits.sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    p->limits.sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    p->limits.sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    p->limits.storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    p->limits.maxSampleMaskWords = 1;
    p->limits.timestampComputeAndGraphics = VK_FALSE;
    p->limits.timestampPeriod = 1.0f;
    p->limits.maxClipDistances = 8;
    p->limits.maxCullDistances = 8;
    p->limits.maxCombinedClipAndCullDistances = 8;
    p->limits.discreteQueuePriorities = 1;
    p->limits.pointSizeRange[0] = 1.0f;
    p->limits.pointSizeRange[1] = 1.0f;
    p->limits.lineWidthRange[0] = 1.0f;
    p->limits.lineWidthRange[1] = 1.0f;
    p->limits.pointSizeGranularity = 1.0f;
    p->limits.lineWidthGranularity = 1.0f;
    p->limits.strictLines = VK_TRUE;
    p->limits.standardSampleLocations = VK_TRUE;
    p->limits.optimalBufferCopyOffsetAlignment = 4;
    p->limits.optimalBufferCopyRowPitchAlignment = 4;
    p->limits.nonCoherentAtomSize = 64;
}

static void cb_stub_memory_properties(VkPhysicalDeviceMemoryProperties *p) {
    memset(p, 0, sizeof *p);
    p->memoryTypeCount = 1;
    p->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    p->memoryTypes[0].heapIndex = 0;
    p->memoryHeapCount = 1;
    p->memoryHeaps[0].size = 256ull * 1024ull * 1024ull;
    p->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

static void cb_stub_queue_family(VkQueueFamilyProperties *p) {
    memset(p, 0, sizeof *p);
    p->queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                    VK_QUEUE_TRANSFER_BIT;
    p->queueCount = 1;
    p->timestampValidBits = 0;
    p->minImageTransferGranularity.width = 1;
    p->minImageTransferGranularity.height = 1;
    p->minImageTransferGranularity.depth = 1;
}

static VkFormatProperties cb_stub_format_properties(VkFormat format) {
    VkFormatProperties p;
    memset(&p, 0, sizeof p);

    VkFormatFeatureFlags sampled =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    VkFormatFeatureFlags color = sampled |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;

    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            p.linearTilingFeatures = sampled;
            p.optimalTilingFeatures = color;
            p.bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
            break;
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            p.linearTilingFeatures = sampled;
            p.optimalTilingFeatures = sampled;
            p.bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
            break;
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
            p.optimalTilingFeatures =
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            break;
        default:
            break;
    }

    return p;
}

static VkResult cb_stub_image_format_properties(VkFormat format,
                                                VkImageFormatProperties *p) {
    VkFormatProperties fp = cb_stub_format_properties(format);
    if (!fp.linearTilingFeatures && !fp.optimalTilingFeatures && !fp.bufferFeatures)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    memset(p, 0, sizeof *p);
    p->maxExtent.width = 4096;
    p->maxExtent.height = 4096;
    p->maxExtent.depth = 256;
    p->maxMipLevels = 12;
    p->maxArrayLayers = 256;
    p->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    p->maxResourceSize = 256ull * 1024ull * 1024ull;
    return VK_SUCCESS;
}

static VkResult cb_stub_memory_requirements(uint16_t *out_opcode,
                                            void **out_reply,
                                            uint32_t *out_reply_len) {
    VkMemoryRequirements req;
    memset(&req, 0, sizeof req);
    req.size = 4096;
    req.alignment = 256;
    req.memoryTypeBits = 1;

    cb_writer_t w;
    cb_writer_init_heap(&w, sizeof req);
    cb_w_bytes(&w, &req, sizeof req);
    if (w.overflow) {
        cb_writer_dispose(&w);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                        out_reply_len);
    return VK_SUCCESS;
}

static VkResult cb_stub_many_ids(uint32_t count, uint16_t *out_opcode,
                                 void **out_reply, uint32_t *out_reply_len) {
    cb_writer_t w;
    cb_writer_init_heap(&w, count * sizeof(uint64_t));
    for (uint32_t i = 0; i < count; ++i)
        cb_w_u64(&w, cb_next_id());
    if (w.overflow) {
        cb_writer_dispose(&w);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                        out_reply_len);
    return VK_SUCCESS;
}

VkResult cb_stub_rpc_call(uint16_t opcode, const void *payload, uint32_t len,
                          uint16_t *out_opcode, void **out_reply,
                          uint32_t *out_reply_len) {
    cb_reader_t r;
    cb_reader_init(&r, payload, len);

    CB_D("stub rpc opcode=0x%04x len=%u", opcode, len);

    switch (opcode) {
        case CB_OP_CREATE_INSTANCE:
        case CB_OP_CREATE_DEVICE:
        case CB_OP_GET_DEVICE_QUEUE:
        case CB_OP_ALLOCATE_MEMORY:
        case CB_OP_CREATE_BUFFER:
        case CB_OP_CREATE_IMAGE:
        case CB_OP_CREATE_IMAGE_VIEW:
        case CB_OP_CREATE_SAMPLER:
        case CB_OP_CREATE_SHADER_MODULE:
        case CB_OP_CREATE_PIPELINE_CACHE:
        case CB_OP_CREATE_DESC_SET_LAYOUT:
        case CB_OP_CREATE_DESC_POOL:
        case CB_OP_CREATE_PIPELINE_LAYOUT:
        case CB_OP_CREATE_RENDER_PASS:
        case CB_OP_CREATE_FRAMEBUFFER:
        case CB_OP_CREATE_COMMAND_POOL:
        case CB_OP_CREATE_FENCE:
        case CB_OP_CREATE_SEMAPHORE:
        case CB_OP_CREATE_EVENT:
        case CB_OP_CREATE_SWAPCHAIN:
            return cb_stub_id_reply(out_opcode, out_reply, out_reply_len);

        case CB_OP_ENUMERATE_PHYSICAL_DEVICES: {
            cb_writer_t w;
            cb_writer_init_heap(&w, 16);
            cb_w_u32(&w, 1);
            cb_w_u64(&w, 1);
            if (w.overflow) {
                cb_writer_dispose(&w);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_PD_PROPERTIES: {
            VkPhysicalDeviceProperties props;
            cb_stub_physical_device_properties(&props);
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof props);
            cb_w_bytes(&w, &props, sizeof props);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_PD_FEATURES: {
            VkPhysicalDeviceFeatures features;
            memset(&features, 0, sizeof features);
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof features);
            cb_w_bytes(&w, &features, sizeof features);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_PD_MEMORY_PROPS: {
            VkPhysicalDeviceMemoryProperties memprops;
            cb_stub_memory_properties(&memprops);
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof memprops);
            cb_w_bytes(&w, &memprops, sizeof memprops);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_PD_QUEUE_FAMILY_PROPS: {
            VkQueueFamilyProperties qfp;
            cb_stub_queue_family(&qfp);
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof qfp + sizeof(uint32_t));
            cb_w_u32(&w, 1);
            cb_w_bytes(&w, &qfp, sizeof qfp);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_PD_FORMAT_PROPS: {
            cb_r_u64(&r);
            VkFormat format = (VkFormat)cb_r_u32(&r);
            VkFormatProperties props = cb_stub_format_properties(format);
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof props);
            cb_w_bytes(&w, &props, sizeof props);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_PD_IMAGE_FORMAT_PROPS: {
            cb_r_u64(&r);
            VkFormat format = (VkFormat)cb_r_u32(&r);
            VkImageFormatProperties props;
            VkResult vr = cb_stub_image_format_properties(format, &props);
            if (vr != VK_SUCCESS) return vr;
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof props);
            cb_w_bytes(&w, &props, sizeof props);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_BUFFER_MEM_REQS:
        case CB_OP_GET_IMAGE_MEM_REQS:
            return cb_stub_memory_requirements(out_opcode, out_reply,
                                               out_reply_len);

        case CB_OP_READ_MEMORY: {
            cb_r_u64(&r);
            cb_r_u64(&r);
            uint64_t size = cb_r_u64(&r);
            if (size > CB_MAX_FRAME_BYTES) return VK_ERROR_OUT_OF_HOST_MEMORY;
            cb_writer_t w;
            cb_writer_init_heap(&w, (size_t)size);
            if (cb_writer_reserve(&w, (size_t)size)) {
                memset(w.buf + w.pos, 0, (size_t)size);
                w.pos += (size_t)size;
            }
            if (w.overflow) {
                cb_writer_dispose(&w);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_ALLOCATE_COMMAND_BUFFERS: {
            cb_r_u64(&r);
            cb_r_u64(&r);
            cb_r_u32(&r);
            uint32_t count = cb_r_u32(&r);
            return cb_stub_many_ids(count, out_opcode, out_reply, out_reply_len);
        }

        case CB_OP_ALLOCATE_DESC_SETS: {
            cb_r_u64(&r);
            cb_r_u64(&r);
            uint32_t count = cb_r_u32(&r);
            return cb_stub_many_ids(count, out_opcode, out_reply, out_reply_len);
        }

        case CB_OP_CREATE_GRAPHICS_PIPELINES:
        case CB_OP_CREATE_COMPUTE_PIPELINES: {
            cb_r_u64(&r);
            cb_r_u64(&r);
            uint32_t count = cb_r_u32(&r);
            return cb_stub_many_ids(count, out_opcode, out_reply, out_reply_len);
        }

        case CB_OP_GET_SURFACE_SUPPORT: {
            uint32_t supported = VK_TRUE;
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof supported);
            cb_w_bytes(&w, &supported, sizeof supported);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_SURFACE_CAPABILITIES: {
            VkSurfaceCapabilitiesKHR caps;
            memset(&caps, 0, sizeof caps);
            caps.minImageCount = 2;
            caps.maxImageCount = 3;
            caps.currentExtent.width = 1280;
            caps.currentExtent.height = 720;
            caps.minImageExtent.width = 64;
            caps.minImageExtent.height = 64;
            caps.maxImageExtent.width = 4096;
            caps.maxImageExtent.height = 4096;
            caps.maxImageArrayLayers = 1;
            caps.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            caps.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            caps.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            caps.supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof caps);
            cb_w_bytes(&w, &caps, sizeof caps);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_SURFACE_FORMATS: {
            VkSurfaceFormatKHR fmt;
            memset(&fmt, 0, sizeof fmt);
            fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
            fmt.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof fmt + sizeof(uint32_t));
            cb_w_u32(&w, 1);
            cb_w_bytes(&w, &fmt, sizeof fmt);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_SURFACE_PRESENT_MODES: {
            cb_writer_t w;
            cb_writer_init_heap(&w, 8);
            cb_w_u32(&w, 1);
            cb_w_u32(&w, VK_PRESENT_MODE_FIFO_KHR);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_GET_SWAPCHAIN_IMAGES: {
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof(uint32_t) + 2 * sizeof(uint64_t));
            cb_w_u32(&w, 2);
            cb_w_u64(&w, cb_next_id());
            cb_w_u64(&w, cb_next_id());
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_ACQUIRE_NEXT_IMAGE: {
            uint32_t image_index = 0;
            cb_writer_t w;
            cb_writer_init_heap(&w, sizeof image_index);
            cb_w_bytes(&w, &image_index, sizeof image_index);
            cb_stub_write_reply(CB_OP_GENERIC_REPLY, &w, out_opcode, out_reply,
                                out_reply_len);
            return VK_SUCCESS;
        }

        case CB_OP_DESTROY_INSTANCE:
        case CB_OP_DESTROY_DEVICE:
        case CB_OP_FREE_MEMORY:
        case CB_OP_WRITE_MEMORY:
        case CB_OP_DESTROY_BUFFER:
        case CB_OP_BIND_BUFFER_MEMORY:
        case CB_OP_DESTROY_IMAGE:
        case CB_OP_BIND_IMAGE_MEMORY:
        case CB_OP_DESTROY_IMAGE_VIEW:
        case CB_OP_DESTROY_SAMPLER:
        case CB_OP_DESTROY_SHADER_MODULE:
        case CB_OP_DESTROY_PIPELINE_CACHE:
        case CB_OP_DESTROY_DESC_SET_LAYOUT:
        case CB_OP_DESTROY_DESC_POOL:
        case CB_OP_FREE_DESC_SETS:
        case CB_OP_UPDATE_DESC_SETS:
        case CB_OP_DESTROY_PIPELINE_LAYOUT:
        case CB_OP_DESTROY_RENDER_PASS:
        case CB_OP_DESTROY_FRAMEBUFFER:
        case CB_OP_DESTROY_PIPELINE:
        case CB_OP_DESTROY_COMMAND_POOL:
        case CB_OP_RESET_COMMAND_POOL:
        case CB_OP_FREE_COMMAND_BUFFERS:
        case CB_OP_BEGIN_COMMAND_BUFFER:
        case CB_OP_RECORD_COMMAND_STREAM:
        case CB_OP_END_COMMAND_BUFFER:
        case CB_OP_RESET_COMMAND_BUFFER:
        case CB_OP_QUEUE_SUBMIT:
        case CB_OP_QUEUE_PRESENT:
        case CB_OP_DESTROY_FENCE:
        case CB_OP_WAIT_FOR_FENCES:
        case CB_OP_RESET_FENCES:
        case CB_OP_GET_FENCE_STATUS:
        case CB_OP_DESTROY_SEMAPHORE:
        case CB_OP_DESTROY_EVENT:
        case CB_OP_DESTROY_SURFACE:
        case CB_OP_DESTROY_SWAPCHAIN:
        case CB_OP_DEVICE_WAIT_IDLE:
        case CB_OP_QUEUE_WAIT_IDLE:
            return cb_stub_empty_reply(out_opcode, out_reply, out_reply_len);

        default:
            CB_W("stub backend does not implement opcode=0x%04x", opcode);
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
}
