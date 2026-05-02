/*
 * CheeseBridge host: shared internal types.
 *
 * The host is a Cocoa app that listens on a TCP socket for guest ICD
 * connections. Each connection runs on its own dispatch thread; that
 * thread reads a frame, dispatches it, and writes a reply. All Vulkan
 * calls go through dynamically-loaded MoltenVK (libMoltenVK.dylib).
 *
 * Resource lifetimes:
 *   The guest hands us 64-bit ids it allocated. We translate those ids to
 *   the corresponding Vk* handle via the global resource table. When the
 *   guest sends DESTROY_*, we call the matching MoltenVK destroyer and
 *   drop the entry.
 *
 * Threading:
 *   - The listen socket and Cocoa main loop run on the main thread.
 *   - Each guest connection dispatches on its own pthread.
 *   - Window / swapchain present operations are bounced back to the main
 *     thread via dispatch_async(dispatch_get_main_queue(), ^{ ... }).
 */
#ifndef CHEESEBRIDGE_HOST_H
#define CHEESEBRIDGE_HOST_H

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include "cheesebridge_proto.h"
#include "cheesebridge_wire.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __OBJC__
#  import <Cocoa/Cocoa.h>
#  import <QuartzCore/CAMetalLayer.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Logging ------------------------------------------------------------ */

typedef enum host_log_level {
    HL_ERROR = 0, HL_WARN, HL_INFO, HL_DEBUG, HL_TRACE
} host_log_level_t;

void host_log(host_log_level_t lvl, const char *fmt, ...);
#define HE(...) host_log(HL_ERROR, __VA_ARGS__)
#define HW(...) host_log(HL_WARN,  __VA_ARGS__)
#define HI(...) host_log(HL_INFO,  __VA_ARGS__)
#define HD(...) host_log(HL_DEBUG, __VA_ARGS__)
#define HT(...) host_log(HL_TRACE, __VA_ARGS__)

/* ---- MoltenVK function table ------------------------------------------- */

/*
 * We resolve every Vulkan entry point we use from libMoltenVK.dylib via
 * vkGetInstanceProcAddr. The result is stored on a per-instance / per-device
 * function table. The table mirrors the Vulkan ABI exactly so call sites
 * read like normal Vulkan code: vk->CreateBuffer(...) etc.
 */

typedef struct host_global_funcs {
    void *libmvk;       /* dlopen handle for libMoltenVK.dylib */
    PFN_vkGetInstanceProcAddr             GetInstanceProcAddr;
    PFN_vkEnumerateInstanceVersion        EnumerateInstanceVersion;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance                  CreateInstance;
} host_global_funcs_t;

typedef struct host_instance_funcs {
    PFN_vkDestroyInstance                 DestroyInstance;
    PFN_vkEnumeratePhysicalDevices        EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties     GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures       GetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceImageFormatProperties GetPhysicalDeviceImageFormatProperties;
    PFN_vkCreateDevice                    CreateDevice;
    PFN_vkGetDeviceProcAddr               GetDeviceProcAddr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;

    PFN_vkGetPhysicalDeviceSurfaceSupportKHR      GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR      GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkDestroySurfaceKHR                       DestroySurfaceKHR;
    /* MoltenVK-specific Metal surface entry */
    PFN_vkCreateMetalSurfaceEXT                   CreateMetalSurfaceEXT;
} host_instance_funcs_t;

typedef struct host_device_funcs {
    PFN_vkDestroyDevice                   DestroyDevice;
    PFN_vkGetDeviceQueue                  GetDeviceQueue;
    PFN_vkDeviceWaitIdle                  DeviceWaitIdle;
    PFN_vkQueueWaitIdle                   QueueWaitIdle;
    PFN_vkAllocateMemory                  AllocateMemory;
    PFN_vkFreeMemory                      FreeMemory;
    PFN_vkMapMemory                       MapMemory;
    PFN_vkUnmapMemory                     UnmapMemory;
    PFN_vkFlushMappedMemoryRanges         FlushMappedMemoryRanges;
    PFN_vkInvalidateMappedMemoryRanges    InvalidateMappedMemoryRanges;
    PFN_vkCreateBuffer                    CreateBuffer;
    PFN_vkDestroyBuffer                   DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements     GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory                BindBufferMemory;
    PFN_vkCreateImage                     CreateImage;
    PFN_vkDestroyImage                    DestroyImage;
    PFN_vkGetImageMemoryRequirements      GetImageMemoryRequirements;
    PFN_vkBindImageMemory                 BindImageMemory;
    PFN_vkCreateImageView                 CreateImageView;
    PFN_vkDestroyImageView                DestroyImageView;
    PFN_vkCreateSampler                   CreateSampler;
    PFN_vkDestroySampler                  DestroySampler;
    PFN_vkCreateShaderModule              CreateShaderModule;
    PFN_vkDestroyShaderModule             DestroyShaderModule;
    PFN_vkCreatePipelineCache             CreatePipelineCache;
    PFN_vkDestroyPipelineCache            DestroyPipelineCache;
    PFN_vkCreatePipelineLayout            CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout           DestroyPipelineLayout;
    PFN_vkCreateGraphicsPipelines         CreateGraphicsPipelines;
    PFN_vkCreateComputePipelines          CreateComputePipelines;
    PFN_vkDestroyPipeline                 DestroyPipeline;
    PFN_vkCreateRenderPass                CreateRenderPass;
    PFN_vkDestroyRenderPass               DestroyRenderPass;
    PFN_vkCreateFramebuffer               CreateFramebuffer;
    PFN_vkDestroyFramebuffer              DestroyFramebuffer;
    PFN_vkCreateDescriptorSetLayout       CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout      DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool            CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool           DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets          AllocateDescriptorSets;
    PFN_vkFreeDescriptorSets              FreeDescriptorSets;
    PFN_vkUpdateDescriptorSets            UpdateDescriptorSets;
    PFN_vkCreateCommandPool               CreateCommandPool;
    PFN_vkDestroyCommandPool              DestroyCommandPool;
    PFN_vkResetCommandPool                ResetCommandPool;
    PFN_vkAllocateCommandBuffers          AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers              FreeCommandBuffers;
    PFN_vkBeginCommandBuffer              BeginCommandBuffer;
    PFN_vkEndCommandBuffer                EndCommandBuffer;
    PFN_vkResetCommandBuffer              ResetCommandBuffer;

    /* vkCmd* — used by command stream replay */
    PFN_vkCmdBeginRenderPass              CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass                CmdEndRenderPass;
    PFN_vkCmdNextSubpass                  CmdNextSubpass;
    PFN_vkCmdBindPipeline                 CmdBindPipeline;
    PFN_vkCmdBindVertexBuffers            CmdBindVertexBuffers;
    PFN_vkCmdBindIndexBuffer              CmdBindIndexBuffer;
    PFN_vkCmdBindDescriptorSets           CmdBindDescriptorSets;
    PFN_vkCmdPushConstants                CmdPushConstants;
    PFN_vkCmdSetViewport                  CmdSetViewport;
    PFN_vkCmdSetScissor                   CmdSetScissor;
    PFN_vkCmdSetLineWidth                 CmdSetLineWidth;
    PFN_vkCmdSetDepthBias                 CmdSetDepthBias;
    PFN_vkCmdSetBlendConstants            CmdSetBlendConstants;
    PFN_vkCmdSetStencilCompareMask        CmdSetStencilCompareMask;
    PFN_vkCmdSetStencilWriteMask          CmdSetStencilWriteMask;
    PFN_vkCmdSetStencilReference          CmdSetStencilReference;
    PFN_vkCmdDraw                         CmdDraw;
    PFN_vkCmdDrawIndexed                  CmdDrawIndexed;
    PFN_vkCmdDrawIndirect                 CmdDrawIndirect;
    PFN_vkCmdDrawIndexedIndirect          CmdDrawIndexedIndirect;
    PFN_vkCmdDispatch                     CmdDispatch;
    PFN_vkCmdDispatchIndirect             CmdDispatchIndirect;
    PFN_vkCmdCopyBuffer                   CmdCopyBuffer;
    PFN_vkCmdCopyImage                    CmdCopyImage;
    PFN_vkCmdCopyBufferToImage            CmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer            CmdCopyImageToBuffer;
    PFN_vkCmdBlitImage                    CmdBlitImage;
    PFN_vkCmdPipelineBarrier              CmdPipelineBarrier;
    PFN_vkCmdClearColorImage              CmdClearColorImage;
    PFN_vkCmdClearAttachments             CmdClearAttachments;
    PFN_vkCmdFillBuffer                   CmdFillBuffer;
    PFN_vkCmdUpdateBuffer                 CmdUpdateBuffer;

    PFN_vkCreateFence                     CreateFence;
    PFN_vkDestroyFence                    DestroyFence;
    PFN_vkWaitForFences                   WaitForFences;
    PFN_vkResetFences                     ResetFences;
    PFN_vkGetFenceStatus                  GetFenceStatus;
    PFN_vkCreateSemaphore                 CreateSemaphore;
    PFN_vkDestroySemaphore                DestroySemaphore;
    PFN_vkCreateEvent                     CreateEvent;
    PFN_vkDestroyEvent                    DestroyEvent;

    PFN_vkQueueSubmit                     QueueSubmit;
    PFN_vkCreateSwapchainKHR              CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR             DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR           GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR             AcquireNextImageKHR;
    PFN_vkQueuePresentKHR                 QueuePresentKHR;
} host_device_funcs_t;

extern host_global_funcs_t g_vk;

VkResult host_load_moltenvk(void);
void     host_load_instance_funcs(VkInstance instance, host_instance_funcs_t *out);
void     host_load_device_funcs  (VkInstance instance, VkDevice device,
                                  host_device_funcs_t *out);

/* ---- Resource table ---------------------------------------------------- */

typedef enum host_obj_kind {
    HK_INSTANCE = 1, HK_PHYSICAL_DEVICE, HK_DEVICE, HK_QUEUE,
    HK_DEVICE_MEMORY, HK_BUFFER, HK_BUFFER_VIEW, HK_IMAGE, HK_IMAGE_VIEW,
    HK_SAMPLER, HK_SHADER_MODULE, HK_PIPELINE, HK_PIPELINE_LAYOUT,
    HK_PIPELINE_CACHE, HK_RENDER_PASS, HK_FRAMEBUFFER,
    HK_DESC_SET_LAYOUT, HK_DESC_POOL, HK_DESC_SET,
    HK_COMMAND_POOL, HK_COMMAND_BUFFER,
    HK_FENCE, HK_SEMAPHORE, HK_EVENT,
    HK_SURFACE, HK_SWAPCHAIN,
    HK_HOST_INSTANCE, HK_HOST_DEVICE   /* host-side aux records */
} host_obj_kind_t;

typedef struct host_obj {
    host_obj_kind_t kind;
    void           *ptr;        /* the actual Vk* handle, or aux record */
    cb_remote_id_t  parent_id;  /* the device/instance that owns this */
} host_obj_t;

void  host_table_init(void);
void  host_table_put(cb_remote_id_t id, host_obj_kind_t k, void *ptr,
                     cb_remote_id_t parent_id);
void *host_table_get(cb_remote_id_t id, host_obj_kind_t k);
void  host_table_drop(cb_remote_id_t id);

/* Strongly-typed convenience wrappers. */
#define HT_GET(ID, K)  host_table_get((ID), (K))

/* ---- Per-connection record --------------------------------------------- */

typedef struct host_conn {
    int             fd;
    pthread_t       thread;
    pthread_mutex_t write_lock;
} host_conn_t;

void host_conn_dispatch_loop(host_conn_t *c);

/* ---- Reply helpers ----------------------------------------------------- */

void host_reply_ok      (host_conn_t *c, uint32_t seq);
void host_reply_id      (host_conn_t *c, uint32_t seq, cb_remote_id_t id);
void host_reply_bytes   (host_conn_t *c, uint32_t seq,
                         const void *bytes, uint32_t n);
void host_reply_fail    (host_conn_t *c, uint32_t seq, VkResult vr);
void host_reply_writer  (host_conn_t *c, uint32_t seq, cb_writer_t *w);

/* Per-instance/per-device function tables registered in resource table. */
typedef struct host_instance_rec {
    VkInstance              vk;
    host_instance_funcs_t   ifn;
    /* enumerated physical devices */
    uint32_t                pd_count;
    VkPhysicalDevice       *pds;
} host_instance_rec_t;

typedef struct host_device_rec {
    VkDevice            vk;
    host_device_funcs_t fn;
    VkPhysicalDevice    pd;
    host_instance_rec_t *inst;
} host_device_rec_t;

/* Convenience accessor: given an instance id, returns its rec. */
host_instance_rec_t *host_instance_for(cb_remote_id_t id);
host_device_rec_t   *host_device_for  (cb_remote_id_t id);

/* ---- Command stream replay --------------------------------------------- */

VkResult host_replay_command_stream(host_device_rec_t *dev,
                                    VkCommandBuffer cb,
                                    const void *bytes, uint32_t len);

/* ---- Window / swapchain hooks ------------------------------------------ */

#ifdef __OBJC__
NSWindow      *host_create_window(uint32_t width, uint32_t height);
CAMetalLayer  *host_window_layer(NSWindow *w);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHEESEBRIDGE_HOST_H */
