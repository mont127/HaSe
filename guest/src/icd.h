/*
 * CheeseBridge guest ICD: internal types and helpers shared by every
 * vk_*.c entry-point file.
 *
 * The ICD presents itself to the Vulkan loader through the standard
 * vk_icdGetInstanceProcAddr / vk_icdNegotiateLoaderICDInterfaceVersion
 * entry points. All Vulkan handles we hand back are pointers to our own
 * internal structures (cb_instance, cb_device, ...). The first machine
 * word of every dispatchable handle struct is a VK_LOADER_DATA slot so
 * the Vulkan loader's dispatcher works.
 *
 * Every public Vulkan call ultimately goes through cb_rpc_call(), which
 * frames a request, ships it over the transport, and blocks on the reply.
 * vkCmd* recording is the exception: those calls are appended to a per
 * command-buffer cb_writer and shipped wholesale at vkEndCommandBuffer /
 * vkQueueSubmit time.
 */
#ifndef CHEESEBRIDGE_GUEST_ICD_H
#define CHEESEBRIDGE_GUEST_ICD_H

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vk_layer.h>

#include "cheesebridge_proto.h"
#include "cheesebridge_wire.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Logging -------------------------------------------------------------- */

typedef enum cb_log_level {
    CB_LOG_ERROR = 0,
    CB_LOG_WARN  = 1,
    CB_LOG_INFO  = 2,
    CB_LOG_DEBUG = 3,
    CB_LOG_TRACE = 4
} cb_log_level_t;

void cb_log(cb_log_level_t lvl, const char *fmt, ...);
#define CB_E(...) cb_log(CB_LOG_ERROR, __VA_ARGS__)
#define CB_W(...) cb_log(CB_LOG_WARN,  __VA_ARGS__)
#define CB_I(...) cb_log(CB_LOG_INFO,  __VA_ARGS__)
#define CB_D(...) cb_log(CB_LOG_DEBUG, __VA_ARGS__)
#define CB_T(...) cb_log(CB_LOG_TRACE, __VA_ARGS__)

/* ---- Transport / RPC ------------------------------------------------------ */

/* One global connection per process. The Vulkan loader is single-process. */
typedef struct cb_transport {
    int             fd;
    pthread_mutex_t lock;        /* serializes request/reply pairs */
    atomic_uint     next_seq;
    bool            connected;
} cb_transport_t;

cb_transport_t *cb_transport_get(void);
VkResult        cb_transport_connect(void);
void            cb_transport_disconnect(void);

/*
 * Send a request, wait for the matching reply on the same fd. The reply
 * payload (if any) is returned via *out_reply / *out_reply_len; caller
 * must free(*out_reply). out_opcode receives the reply opcode (used to
 * distinguish CB_OP_FAIL_REPLY from a normal reply).
 */
VkResult cb_rpc_call(uint16_t opcode, const void *payload, uint32_t len,
                     uint16_t *out_opcode, void **out_reply,
                     uint32_t *out_reply_len);

/* Convenience: send + free reply. Returns VkResult decoded from FAIL_REPLY. */
VkResult cb_rpc_call_void(uint16_t opcode, const void *payload, uint32_t len);

/* Async (no reply). Used for destroy_* paths to avoid a round trip. */
VkResult cb_rpc_send_async(uint16_t opcode, const void *payload, uint32_t len);

/*
 * Phase 2 local stub backend. The ICD uses this when no CHEESEBRIDGE_HOST is
 * configured, so Vulkan loader tools can discover a controlled fake device
 * before the macOS renderer exists.
 */
bool     cb_stub_mode_enabled(void);
VkResult cb_stub_rpc_call(uint16_t opcode, const void *payload, uint32_t len,
                          uint16_t *out_opcode, void **out_reply,
                          uint32_t *out_reply_len);

/* ---- Handles -------------------------------------------------------------- */

/*
 * Each dispatchable Vulkan handle (VkInstance, VkPhysicalDevice, VkDevice,
 * VkQueue, VkCommandBuffer) must have its first 8 bytes be a VK_LOADER_DATA
 * slot initialised by set_loader_magic_value(). That's what the loader
 * trampoline dereferences to find its own dispatch table.
 *
 * Non-dispatchable handles (VkBuffer, VkImage, ...) are 64-bit opaque ids
 * we allocate ourselves; we just wrap a struct in a pointer-cast.
 */

#define CB_DISPATCHABLE_HEADER VK_LOADER_DATA loader_data;

/* 64-bit id namespace for objects living on the host.
 * cb_remote_id_t is defined in protocol/cheesebridge_proto.h (the wire-level
 * type) — included transitively via the wire header. */
#define CB_NULL_ID 0ull

/* atomic next-id allocator (per-instance) */
cb_remote_id_t cb_next_id(void);

/* ---- Object types --------------------------------------------------------- */

typedef struct cb_instance {
    CB_DISPATCHABLE_HEADER
    cb_remote_id_t   remote_id;
    uint32_t         api_version;
    /* cached enumerated physical devices */
    uint32_t         pd_count;
    struct cb_physical_device **pds;
} cb_instance_t;

typedef struct cb_physical_device {
    CB_DISPATCHABLE_HEADER
    cb_instance_t   *instance;
    cb_remote_id_t   remote_id;
    /* lazy-cached property snapshots */
    bool             props_cached;
    VkPhysicalDeviceProperties props;
    bool             feats_cached;
    VkPhysicalDeviceFeatures   feats;
    bool             memprops_cached;
    VkPhysicalDeviceMemoryProperties memprops;
    uint32_t         queue_family_count;
    VkQueueFamilyProperties *queue_families;
} cb_physical_device_t;

typedef struct cb_device {
    CB_DISPATCHABLE_HEADER
    cb_physical_device_t *pd;
    cb_remote_id_t        remote_id;
    /* per-family queue cache */
    struct cb_queue     **queues;
    uint32_t              queue_count;
} cb_device_t;

typedef struct cb_queue {
    CB_DISPATCHABLE_HEADER
    cb_device_t   *device;
    cb_remote_id_t remote_id;
    uint32_t       family;
    uint32_t       index;
} cb_queue_t;

/*
 * Command buffers carry a recording cursor. vkCmd* writes into `stream`
 * using the cb_cmd_* opcodes; vkEndCommandBuffer freezes the stream and
 * vkQueueSubmit ships it.
 */
typedef struct cb_command_buffer {
    CB_DISPATCHABLE_HEADER
    cb_device_t       *device;
    cb_remote_id_t     remote_id;
    cb_remote_id_t     pool_id;
    VkCommandBufferLevel level;
    bool               recording;
    cb_writer_t        stream;
} cb_command_buffer_t;

/* Non-dispatchable wrappers (cast through VK_DEFINE_NON_DISPATCHABLE_HANDLE). */
#define CB_NDH_STRUCT(name)                       \
    typedef struct cb_##name {                    \
        cb_remote_id_t remote_id;                 \
        cb_device_t   *device;                    \
    } cb_##name##_t;

CB_NDH_STRUCT(buffer)
CB_NDH_STRUCT(buffer_view)
CB_NDH_STRUCT(image)
CB_NDH_STRUCT(image_view)
CB_NDH_STRUCT(sampler)
CB_NDH_STRUCT(shader_module)
CB_NDH_STRUCT(pipeline)
CB_NDH_STRUCT(pipeline_layout)
CB_NDH_STRUCT(pipeline_cache)
CB_NDH_STRUCT(render_pass)
CB_NDH_STRUCT(framebuffer)
CB_NDH_STRUCT(descriptor_set_layout)
CB_NDH_STRUCT(descriptor_pool)
CB_NDH_STRUCT(descriptor_set)
CB_NDH_STRUCT(command_pool)
CB_NDH_STRUCT(fence)
CB_NDH_STRUCT(semaphore)
CB_NDH_STRUCT(event)
CB_NDH_STRUCT(query_pool)

/* Surface lives at instance scope, not device scope. */
typedef struct cb_surface {
    cb_remote_id_t  remote_id;
    cb_instance_t  *instance;
} cb_surface_t;

typedef struct cb_swapchain {
    cb_remote_id_t  remote_id;
    cb_device_t    *device;
    cb_surface_t   *surface;
    uint32_t        image_count;
    cb_image_t    **images;
    VkFormat        format;
    VkExtent2D      extent;
} cb_swapchain_t;

/*
 * Memory allocations. For HOST_VISIBLE allocations we keep a guest-side
 * mirror buffer so vkMapMemory can hand the user a writable pointer; on
 * vkFlushMappedMemoryRanges / vkUnmapMemory we ship the dirty range to
 * the host via CB_OP_WRITE_MEMORY.
 */
typedef struct cb_device_memory {
    cb_remote_id_t  remote_id;
    cb_device_t    *device;
    VkDeviceSize    size;
    uint32_t        memory_type_index;
    bool            host_visible;
    void           *guest_mirror;   /* malloc'd, size bytes; NULL if device-local */
    void           *mapped_ptr;     /* == guest_mirror while mapped */
    VkDeviceSize    mapped_offset;
    VkDeviceSize    mapped_size;
} cb_device_memory_t;

/* ---- Handle helpers ------------------------------------------------------- */

/* Allocate a dispatchable handle of `size` bytes with the loader header set. */
void *cb_alloc_dispatchable(size_t size);
void  cb_free_dispatchable(void *p);

/* Wrap/unwrap non-dispatchable handles as opaque uint64 pointers. */
#define CB_TO_HANDLE(p)   ((uint64_t)(uintptr_t)(p))
#define CB_FROM_HANDLE(t, h)  ((t *)(uintptr_t)(h))

/* ---- ProcAddr table ------------------------------------------------------- */

/* Defined in icd_loader.c; consumed via vkGetInstance/DeviceProcAddr. */
PFN_vkVoidFunction cb_lookup_instance_proc(const char *name);
PFN_vkVoidFunction cb_lookup_device_proc  (const char *name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHEESEBRIDGE_GUEST_ICD_H */
