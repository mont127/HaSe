/*
 * CheeseBridge wire protocol.
 *
 * Shared by the Linux guest ICD and the macOS host renderer. This header is
 * pure C99 with no external dependencies so it can be included from both the
 * Linux build (gcc/clang) and the macOS build (clang/Objective-C++).
 *
 * Frame layout (little-endian on the wire):
 *
 *   uint32  magic     = 0xC4EE5EB1   ('CheeseB1')
 *   uint32  length                     payload byte count
 *   uint16  opcode                     CB_OP_*
 *   uint16  flags                      CB_FLAG_*
 *   uint32  sequence                   request id; reply echoes it
 *   uint8[length]  payload             opcode-specific
 *
 * The header is exactly 16 bytes. Replies always carry CB_FLAG_REPLY and the
 * same sequence number as the request. CB_OP_FAIL_REPLY is used when the
 * host wants to surface a VkResult or a transport-level error.
 */

#ifndef CHEESEBRIDGE_PROTO_H
#define CHEESEBRIDGE_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CB_PROTO_MAGIC      0xC4EE5EB1u
#define CB_PROTO_VERSION    1u
#define CB_HEADER_SIZE      16u
#define CB_MAX_FRAME_BYTES  (64u * 1024u * 1024u)   /* 64 MiB hard cap */

#define CB_FLAG_REPLY       0x0001
#define CB_FLAG_ASYNC       0x0002      /* fire-and-forget; no reply expected */
#define CB_FLAG_BULK        0x0004      /* large payload (memory upload, etc.) */

typedef struct cb_frame_header {
    uint32_t magic;
    uint32_t length;
    uint16_t opcode;
    uint16_t flags;
    uint32_t sequence;
} cb_frame_header_t;

/*
 * Opcode space. Reserve ranges so that adding a new family does not collide.
 * Keep these stable; bumping CB_PROTO_VERSION is required for any breaking
 * change to an existing opcode.
 */
enum cb_opcode {
    /* Session / control */
    CB_OP_HELLO                          = 0x0001,
    CB_OP_HELLO_REPLY                    = 0x0002,
    CB_OP_BYE                            = 0x0003,
    CB_OP_FAIL_REPLY                     = 0x0004,
    CB_OP_GENERIC_REPLY                  = 0x0005,   /* status-only OK reply */

    /* Instance / physical device */
    CB_OP_CREATE_INSTANCE                = 0x0010,
    CB_OP_DESTROY_INSTANCE               = 0x0011,
    CB_OP_ENUMERATE_PHYSICAL_DEVICES     = 0x0012,
    CB_OP_GET_PD_PROPERTIES              = 0x0013,
    CB_OP_GET_PD_FEATURES                = 0x0014,
    CB_OP_GET_PD_QUEUE_FAMILY_PROPS      = 0x0015,
    CB_OP_GET_PD_MEMORY_PROPS            = 0x0016,
    CB_OP_GET_PD_FORMAT_PROPS            = 0x0017,
    CB_OP_GET_PD_IMAGE_FORMAT_PROPS      = 0x0018,

    /* Logical device */
    CB_OP_CREATE_DEVICE                  = 0x0020,
    CB_OP_DESTROY_DEVICE                 = 0x0021,
    CB_OP_GET_DEVICE_QUEUE               = 0x0022,
    CB_OP_DEVICE_WAIT_IDLE               = 0x0023,
    CB_OP_QUEUE_WAIT_IDLE                = 0x0024,

    /* Memory */
    CB_OP_ALLOCATE_MEMORY                = 0x0030,
    CB_OP_FREE_MEMORY                    = 0x0031,
    CB_OP_MAP_MEMORY                     = 0x0032,
    CB_OP_UNMAP_MEMORY                   = 0x0033,
    CB_OP_FLUSH_MAPPED_RANGES            = 0x0034,
    CB_OP_INVALIDATE_MAPPED_RANGES       = 0x0035,
    CB_OP_WRITE_MEMORY                   = 0x0036,   /* CB_FLAG_BULK */
    CB_OP_READ_MEMORY                    = 0x0037,

    /* Buffers */
    CB_OP_CREATE_BUFFER                  = 0x0040,
    CB_OP_DESTROY_BUFFER                 = 0x0041,
    CB_OP_GET_BUFFER_MEM_REQS            = 0x0042,
    CB_OP_BIND_BUFFER_MEMORY             = 0x0043,
    CB_OP_CREATE_BUFFER_VIEW             = 0x0044,
    CB_OP_DESTROY_BUFFER_VIEW            = 0x0045,

    /* Images */
    CB_OP_CREATE_IMAGE                   = 0x0050,
    CB_OP_DESTROY_IMAGE                  = 0x0051,
    CB_OP_GET_IMAGE_MEM_REQS             = 0x0052,
    CB_OP_BIND_IMAGE_MEMORY              = 0x0053,
    CB_OP_CREATE_IMAGE_VIEW              = 0x0054,
    CB_OP_DESTROY_IMAGE_VIEW             = 0x0055,
    CB_OP_CREATE_SAMPLER                 = 0x0056,
    CB_OP_DESTROY_SAMPLER                = 0x0057,

    /* Shaders / pipelines */
    CB_OP_CREATE_SHADER_MODULE           = 0x0060,   /* CB_FLAG_BULK (SPIR-V) */
    CB_OP_DESTROY_SHADER_MODULE          = 0x0061,
    CB_OP_CREATE_PIPELINE_LAYOUT         = 0x0070,
    CB_OP_DESTROY_PIPELINE_LAYOUT        = 0x0071,
    CB_OP_CREATE_GRAPHICS_PIPELINES      = 0x0072,
    CB_OP_CREATE_COMPUTE_PIPELINES       = 0x0073,
    CB_OP_DESTROY_PIPELINE               = 0x0074,
    CB_OP_CREATE_PIPELINE_CACHE          = 0x0075,
    CB_OP_DESTROY_PIPELINE_CACHE         = 0x0076,

    /* Render passes / framebuffers */
    CB_OP_CREATE_RENDER_PASS             = 0x0080,
    CB_OP_DESTROY_RENDER_PASS            = 0x0081,
    CB_OP_CREATE_FRAMEBUFFER             = 0x0082,
    CB_OP_DESTROY_FRAMEBUFFER            = 0x0083,

    /* Descriptors */
    CB_OP_CREATE_DESC_SET_LAYOUT         = 0x0090,
    CB_OP_DESTROY_DESC_SET_LAYOUT        = 0x0091,
    CB_OP_CREATE_DESC_POOL               = 0x0092,
    CB_OP_DESTROY_DESC_POOL              = 0x0093,
    CB_OP_ALLOCATE_DESC_SETS             = 0x0094,
    CB_OP_FREE_DESC_SETS                 = 0x0095,
    CB_OP_UPDATE_DESC_SETS               = 0x0096,

    /* Command pools / buffers */
    CB_OP_CREATE_COMMAND_POOL            = 0x00A0,
    CB_OP_DESTROY_COMMAND_POOL           = 0x00A1,
    CB_OP_RESET_COMMAND_POOL             = 0x00A2,
    CB_OP_ALLOCATE_COMMAND_BUFFERS       = 0x00A3,
    CB_OP_FREE_COMMAND_BUFFERS           = 0x00A4,
    CB_OP_BEGIN_COMMAND_BUFFER           = 0x00A5,
    CB_OP_END_COMMAND_BUFFER             = 0x00A6,
    CB_OP_RESET_COMMAND_BUFFER           = 0x00A7,
    CB_OP_RECORD_COMMAND_STREAM          = 0x00A8,   /* CB_FLAG_BULK */

    /* Sync */
    CB_OP_CREATE_FENCE                   = 0x00B0,
    CB_OP_DESTROY_FENCE                  = 0x00B1,
    CB_OP_WAIT_FOR_FENCES                = 0x00B2,
    CB_OP_RESET_FENCES                   = 0x00B3,
    CB_OP_GET_FENCE_STATUS               = 0x00B4,
    CB_OP_CREATE_SEMAPHORE               = 0x00B5,
    CB_OP_DESTROY_SEMAPHORE              = 0x00B6,
    CB_OP_CREATE_EVENT                   = 0x00B7,
    CB_OP_DESTROY_EVENT                  = 0x00B8,

    /* Submission */
    CB_OP_QUEUE_SUBMIT                   = 0x00C0,
    CB_OP_QUEUE_PRESENT                  = 0x00C1,

    /* WSI */
    CB_OP_CREATE_SURFACE                 = 0x00D0,
    CB_OP_DESTROY_SURFACE                = 0x00D1,
    CB_OP_GET_SURFACE_SUPPORT            = 0x00D2,
    CB_OP_GET_SURFACE_CAPABILITIES       = 0x00D3,
    CB_OP_GET_SURFACE_FORMATS            = 0x00D4,
    CB_OP_GET_SURFACE_PRESENT_MODES      = 0x00D5,
    CB_OP_CREATE_SWAPCHAIN               = 0x00D6,
    CB_OP_DESTROY_SWAPCHAIN              = 0x00D7,
    CB_OP_GET_SWAPCHAIN_IMAGES           = 0x00D8,
    CB_OP_ACQUIRE_NEXT_IMAGE             = 0x00D9
};

/*
 * Recorded command stream opcodes (payload of CB_OP_RECORD_COMMAND_STREAM).
 * The guest captures vkCmd* calls into a tightly-packed buffer and ships them
 * to the host, which replays them into a real VkCommandBuffer via MoltenVK.
 *
 * Each entry begins with a uint16 op + uint16 size (size includes the 4-byte
 * header), making the stream forward-iterable without a manifest.
 */
enum cb_cmd_opcode {
    CB_CMD_BEGIN_RENDER_PASS             = 0x0001,
    CB_CMD_END_RENDER_PASS               = 0x0002,
    CB_CMD_NEXT_SUBPASS                  = 0x0003,
    CB_CMD_BIND_PIPELINE                 = 0x0004,
    CB_CMD_BIND_VERTEX_BUFFERS           = 0x0005,
    CB_CMD_BIND_INDEX_BUFFER             = 0x0006,
    CB_CMD_BIND_DESCRIPTOR_SETS          = 0x0007,
    CB_CMD_PUSH_CONSTANTS                = 0x0008,
    CB_CMD_SET_VIEWPORT                  = 0x0009,
    CB_CMD_SET_SCISSOR                   = 0x000A,
    CB_CMD_SET_LINE_WIDTH                = 0x000B,
    CB_CMD_SET_DEPTH_BIAS                = 0x000C,
    CB_CMD_SET_BLEND_CONSTANTS           = 0x000D,
    CB_CMD_SET_STENCIL_COMPARE_MASK      = 0x000E,
    CB_CMD_SET_STENCIL_WRITE_MASK        = 0x000F,
    CB_CMD_SET_STENCIL_REFERENCE         = 0x0010,
    CB_CMD_DRAW                          = 0x0011,
    CB_CMD_DRAW_INDEXED                  = 0x0012,
    CB_CMD_DRAW_INDIRECT                 = 0x0013,
    CB_CMD_DRAW_INDEXED_INDIRECT         = 0x0014,
    CB_CMD_DISPATCH                      = 0x0015,
    CB_CMD_DISPATCH_INDIRECT             = 0x0016,
    CB_CMD_COPY_BUFFER                   = 0x0017,
    CB_CMD_COPY_IMAGE                    = 0x0018,
    CB_CMD_COPY_BUFFER_TO_IMAGE          = 0x0019,
    CB_CMD_COPY_IMAGE_TO_BUFFER          = 0x001A,
    CB_CMD_BLIT_IMAGE                    = 0x001B,
    CB_CMD_RESOLVE_IMAGE                 = 0x001C,
    CB_CMD_UPDATE_BUFFER                 = 0x001D,
    CB_CMD_FILL_BUFFER                   = 0x001E,
    CB_CMD_CLEAR_COLOR_IMAGE             = 0x001F,
    CB_CMD_CLEAR_DEPTH_STENCIL_IMAGE     = 0x0020,
    CB_CMD_CLEAR_ATTACHMENTS             = 0x0021,
    CB_CMD_PIPELINE_BARRIER              = 0x0022,
    CB_CMD_EXECUTE_COMMANDS              = 0x0023,
    CB_CMD_BEGIN_QUERY                   = 0x0024,
    CB_CMD_END_QUERY                     = 0x0025,
    CB_CMD_RESET_QUERY_POOL              = 0x0026,
    CB_CMD_WRITE_TIMESTAMP               = 0x0027,
    CB_CMD_COPY_QUERY_POOL_RESULTS       = 0x0028
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHEESEBRIDGE_PROTO_H */
