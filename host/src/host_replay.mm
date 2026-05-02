/*
 * CheeseBridge host: command-stream replay.
 *
 * The guest serializes a vkCmd* sequence into a flat byte blob using the
 * encoding documented in guest/src/icd_command.c:
 *
 *   per-entry header:
 *     uint16 op
 *     uint16 size      (total bytes including these 4 header bytes)
 *     ...opcode-specific bytes...
 *
 * This file decodes that stream and re-issues real Vulkan calls against the
 * host VkCommandBuffer through MoltenVK. Phase 3 covers the minimum set
 * needed to drive a clear-color frame end-to-end:
 *
 *   - CB_CMD_PIPELINE_BARRIER  (no-op for now; barriers are state hints)
 *   - CB_CMD_CLEAR_COLOR_IMAGE (the actual pixel-mutating call)
 *   - CB_CMD_FILL_BUFFER       (cheap to add and exercises buffer ids)
 *
 * Other opcodes are logged and skipped without failing the stream so a
 * guest that issues unsupported commands still gets a usable reply rather
 * than a hard VK_ERROR_*. Phase 4 fills in the remaining opcodes.
 */

#include "host.h"

#include <string.h>

/* ---- inline little-endian readers ---------------------------------------- */

typedef struct cmd_reader {
    const uint8_t *p;
    const uint8_t *end;
} cmd_reader_t;

static inline int cr_avail(cmd_reader_t *r, size_t n) {
    return (size_t)(r->end - r->p) >= n;
}
static inline uint16_t cr_u16(cmd_reader_t *r) {
    uint16_t v; memcpy(&v, r->p, 2); r->p += 2; return v;
}
static inline uint32_t cr_u32(cmd_reader_t *r) {
    uint32_t v; memcpy(&v, r->p, 4); r->p += 4; return v;
}
static inline uint64_t cr_u64(cmd_reader_t *r) {
    uint64_t v; memcpy(&v, r->p, 8); r->p += 8; return v;
}
static inline const void *cr_bytes(cmd_reader_t *r, size_t n) {
    const void *b = r->p; r->p += n; return b;
}

/* ---- per-opcode handlers ------------------------------------------------- */

static VkResult replay_clear_color_image(host_device_rec_t *dev,
                                         VkCommandBuffer cb,
                                         cmd_reader_t *r,
                                         uint32_t entry_size) {
    /* layout: u64 image_id | u32 layout | VkClearColorValue (16) |
     *         u32 rangeCount | rangeCount * VkImageSubresourceRange */
    if (entry_size < 4 + 8 + 4 + 16 + 4) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t image_id  = cr_u64(r);
    uint32_t layout    = cr_u32(r);
    if (!cr_avail(r, sizeof(VkClearColorValue) + 4)) return VK_ERROR_INITIALIZATION_FAILED;
    VkClearColorValue color;
    memcpy(&color, cr_bytes(r, sizeof color), sizeof color);
    uint32_t range_count = cr_u32(r);
    if (!cr_avail(r, range_count * sizeof(VkImageSubresourceRange)))
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkImageSubresourceRange *ranges =
        (const VkImageSubresourceRange *)cr_bytes(r, range_count * sizeof *ranges);

    VkImage image = (VkImage)host_table_get(image_id, HK_IMAGE);
    if (!image) {
        host_log(HL_WARN, "replay: ClearColorImage unknown image_id=%llu",
                 (unsigned long long)image_id);
        return VK_SUCCESS;
    }
    if (!dev->fn.CmdClearColorImage) return VK_ERROR_FEATURE_NOT_PRESENT;
    dev->fn.CmdClearColorImage(cb, image, (VkImageLayout)layout, &color,
                               range_count, ranges);
    return VK_SUCCESS;
}

static VkResult replay_pipeline_barrier(host_device_rec_t *dev,
                                        VkCommandBuffer cb,
                                        cmd_reader_t *r,
                                        uint32_t entry_size) {
    /* Layout matches guest cb_vkCmdPipelineBarrier serialization:
     *   u32 src | u32 dst | u32 dep
     *   u32 mb_count | mb_count * VkMemoryBarrier (raw bytes)
     *   u32 bb_count | bb_count * (4*u32 + 3*u64)        (per-buffer barrier)
     *   u32 ib_count | ib_count * (6*u32 + u64 + 5*u32)  (per-image barrier)
     */
    if (!cr_avail(r, 4 + 4 + 4 + 4)) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t src   = cr_u32(r);
    uint32_t dst   = cr_u32(r);
    uint32_t dep   = cr_u32(r);

    uint32_t mb_n = cr_u32(r);
    if (!cr_avail(r, mb_n * sizeof(VkMemoryBarrier) + 4))
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkMemoryBarrier *mb_raw =
        (const VkMemoryBarrier *)cr_bytes(r, mb_n * sizeof(VkMemoryBarrier));

    uint32_t bb_n = cr_u32(r);
    VkBufferMemoryBarrier *bbs = bb_n
        ? (VkBufferMemoryBarrier *)calloc(bb_n, sizeof *bbs) : NULL;
    for (uint32_t i = 0; i < bb_n; ++i) {
        if (!cr_avail(r, 4*4 + 3*8)) { free(bbs); return VK_ERROR_INITIALIZATION_FAILED; }
        bbs[i].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bbs[i].srcAccessMask       = cr_u32(r);
        bbs[i].dstAccessMask       = cr_u32(r);
        bbs[i].srcQueueFamilyIndex = cr_u32(r);
        bbs[i].dstQueueFamilyIndex = cr_u32(r);
        uint64_t buf_id            = cr_u64(r);
        bbs[i].offset              = cr_u64(r);
        bbs[i].size                = cr_u64(r);
        bbs[i].buffer = (VkBuffer)host_table_get(buf_id, HK_BUFFER);
        if (!bbs[i].buffer) {
            host_log(HL_WARN, "barrier: unknown buffer id=%llu", (unsigned long long)buf_id);
        }
    }

    uint32_t ib_n = cr_u32(r);
    VkImageMemoryBarrier *ibs = ib_n
        ? (VkImageMemoryBarrier *)calloc(ib_n, sizeof *ibs) : NULL;
    for (uint32_t i = 0; i < ib_n; ++i) {
        if (!cr_avail(r, 6*4 + 8 + 5*4)) {
            free(bbs); free(ibs); return VK_ERROR_INITIALIZATION_FAILED;
        }
        ibs[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ibs[i].srcAccessMask       = cr_u32(r);
        ibs[i].dstAccessMask       = cr_u32(r);
        ibs[i].oldLayout           = (VkImageLayout)cr_u32(r);
        ibs[i].newLayout           = (VkImageLayout)cr_u32(r);
        ibs[i].srcQueueFamilyIndex = cr_u32(r);
        ibs[i].dstQueueFamilyIndex = cr_u32(r);
        uint64_t img_id            = cr_u64(r);
        ibs[i].subresourceRange.aspectMask     = cr_u32(r);
        ibs[i].subresourceRange.baseMipLevel   = cr_u32(r);
        ibs[i].subresourceRange.levelCount     = cr_u32(r);
        ibs[i].subresourceRange.baseArrayLayer = cr_u32(r);
        ibs[i].subresourceRange.layerCount     = cr_u32(r);
        ibs[i].image = (VkImage)host_table_get(img_id, HK_IMAGE);
        if (!ibs[i].image) {
            host_log(HL_WARN, "barrier: unknown image id=%llu", (unsigned long long)img_id);
        }
    }

    /* Drop barriers whose target handle didn't resolve — issuing them with
     * VK_NULL_HANDLE would crash MoltenVK. Compact in place. */
    uint32_t bb_keep = 0;
    for (uint32_t i = 0; i < bb_n; ++i) if (bbs[i].buffer) bbs[bb_keep++] = bbs[i];
    uint32_t ib_keep = 0;
    for (uint32_t i = 0; i < ib_n; ++i) if (ibs[i].image)  ibs[ib_keep++] = ibs[i];

    if (dev->fn.CmdPipelineBarrier) {
        dev->fn.CmdPipelineBarrier(cb,
            (VkPipelineStageFlags)src, (VkPipelineStageFlags)dst,
            (VkDependencyFlags)dep,
            mb_n, mb_raw,
            bb_keep, bbs,
            ib_keep, ibs);
    }

    free(bbs); free(ibs);
    (void)entry_size;
    return VK_SUCCESS;
}

static VkResult replay_fill_buffer(host_device_rec_t *dev,
                                   VkCommandBuffer cb,
                                   cmd_reader_t *r,
                                   uint32_t entry_size) {
    /* layout: u64 buffer_id | u64 offset | u64 size | u32 data */
    if (entry_size < 4 + 8 + 8 + 8 + 4) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t buf_id = cr_u64(r);
    uint64_t offset = cr_u64(r);
    uint64_t size   = cr_u64(r);
    uint32_t data   = cr_u32(r);
    VkBuffer buf = (VkBuffer)host_table_get(buf_id, HK_BUFFER);
    if (!buf || !dev->fn.CmdFillBuffer) return VK_SUCCESS;
    dev->fn.CmdFillBuffer(cb, buf, offset, size, data);
    return VK_SUCCESS;
}

/* ---- top-level replay loop ----------------------------------------------- */

VkResult host_replay_command_stream(host_device_rec_t *dev,
                                    VkCommandBuffer cb,
                                    const void *bytes, uint32_t len) {
    if (!dev || !cb) return VK_ERROR_DEVICE_LOST;
    if (!bytes || !len) return VK_SUCCESS;

    cmd_reader_t top = { (const uint8_t *)bytes, (const uint8_t *)bytes + len };
    uint32_t handled = 0, skipped = 0;

    while (cr_avail(&top, 4)) {
        const uint8_t *entry_start = top.p;
        uint16_t op   = cr_u16(&top);
        uint16_t size = cr_u16(&top);
        if (size < 4 || (size_t)(top.end - entry_start) < size) {
            host_log(HL_WARN, "replay: malformed entry op=0x%04x size=%u",
                     op, size);
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        cmd_reader_t er = { top.p, entry_start + size };

        VkResult vr = VK_SUCCESS;
        switch (op) {
        case CB_CMD_CLEAR_COLOR_IMAGE:
            vr = replay_clear_color_image(dev, cb, &er, size);
            handled++;
            break;
        case CB_CMD_FILL_BUFFER:
            vr = replay_fill_buffer(dev, cb, &er, size);
            handled++;
            break;
        case CB_CMD_PIPELINE_BARRIER:
            vr = replay_pipeline_barrier(dev, cb, &er, size);
            handled++;
            break;
        default:
            host_log(HL_DEBUG, "replay: skipping unsupported cmd op=0x%04x size=%u",
                     op, size);
            skipped++;
            break;
        }
        if (vr != VK_SUCCESS) return vr;

        top.p = entry_start + size;
    }

    host_log(HL_DEBUG, "replay: handled=%u skipped=%u (%u bytes)",
             handled, skipped, len);
    return VK_SUCCESS;
}
