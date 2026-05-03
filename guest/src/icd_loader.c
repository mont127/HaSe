/*
 * Loader interface and global GetProcAddr table.
 *
 * The Vulkan loader will call vk_icdNegotiateLoaderICDInterfaceVersion()
 * (loader interface v3+) and vk_icdGetInstanceProcAddr() (v2+). Everything
 * the loader hands to the application then comes back to us through the
 * trampoline, dispatched on the loader-magic-tagged dispatchable handle.
 */
#include "icd.h"

#include <string.h>

/* ---- Forward declarations of every entry point we expose ---------------- */

#define CB_VK_FN(name)  VKAPI_ATTR vk_return_##name VKAPI_CALL cb_##name vk_args_##name;

/* The macro above isn't easy to maintain; just declare normally. */
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyInstance(VkInstance, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkEnumeratePhysicalDevices(VkInstance, uint32_t *, VkPhysicalDevice *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkEnumerateInstanceExtensionProperties(const char *, uint32_t *, VkExtensionProperties *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkEnumerateInstanceLayerProperties(uint32_t *, VkLayerProperties *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkEnumerateInstanceVersion(uint32_t *);

VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits, VkImageUsageFlags, VkImageTiling, uint32_t *, VkSparseImageFormatProperties *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice, VkFormat, VkFormatProperties2 *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *, VkImageFormatProperties2 *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkGetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *, VkImageFormatProperties2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *, uint32_t *, VkSparseImageFormatProperties2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceSparseImageFormatProperties2KHR(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *, uint32_t *, VkSparseImageFormatProperties2 *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo *, VkExternalBufferProperties *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceExternalBufferPropertiesKHR(VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo *, VkExternalBufferProperties *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo *, VkExternalFenceProperties *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceExternalFencePropertiesKHR(VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo *, VkExternalFenceProperties *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo *, VkExternalSemaphoreProperties *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo *, VkExternalSemaphoreProperties *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t *, VkLayerProperties *);

VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyDevice(VkDevice, const VkAllocationCallbacks *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkDeviceWaitIdle(VkDevice);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkQueueWaitIdle(VkQueue);

VKAPI_ATTR VkResult VKAPI_CALL cb_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *);
VKAPI_ATTR void     VKAPI_CALL cb_vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **);
VKAPI_ATTR void     VKAPI_CALL cb_vkUnmapMemory(VkDevice, VkDeviceMemory);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkFlushMappedMemoryRanges(VkDevice, uint32_t, const VkMappedMemoryRange *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkInvalidateMappedMemoryRanges(VkDevice, uint32_t, const VkMappedMemoryRange *);

VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);

VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *);
VKAPI_ATTR void     VKAPI_CALL cb_vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateImageView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *, VkImageView *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateSampler(VkDevice, const VkSamplerCreateInfo *, const VkAllocationCallbacks *, VkSampler *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *, const VkAllocationCallbacks *, VkShaderModule *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo *, const VkAllocationCallbacks *, VkPipelineLayout *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo *, const VkAllocationCallbacks *, VkPipelineCache *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyPipelineCache(VkDevice, VkPipelineCache, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo *, const VkAllocationCallbacks *, VkRenderPass *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo *, const VkAllocationCallbacks *, VkFramebuffer *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo *, const VkAllocationCallbacks *, VkDescriptorSetLayout *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo *, const VkAllocationCallbacks *, VkDescriptorPool *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkFreeDescriptorSets(VkDevice, VkDescriptorPool, uint32_t, const VkDescriptorSet *);
VKAPI_ATTR void     VKAPI_CALL cb_vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet *, uint32_t, const VkCopyDescriptorSet *);

VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
VKAPI_ATTR void     VKAPI_CALL cb_vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkEndCommandBuffer(VkCommandBuffer);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags);

/* vkCmd* recording functions live in icd_command.c */
VKAPI_ATTR void VKAPI_CALL cb_vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo *, VkSubpassContents);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdEndRenderPass(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdNextSubpass(VkCommandBuffer, VkSubpassContents);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdBindVertexBuffers(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdBindIndexBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet *, uint32_t, const uint32_t *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdSetViewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdSetScissor(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdSetLineWidth(VkCommandBuffer, float);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdSetDepthBias(VkCommandBuffer, float, float, float);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdSetBlendConstants(VkCommandBuffer, const float[4]);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdSetStencilCompareMask(VkCommandBuffer, VkStencilFaceFlags, uint32_t);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdSetStencilWriteMask(VkCommandBuffer, VkStencilFaceFlags, uint32_t);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdSetStencilReference(VkCommandBuffer, VkStencilFaceFlags, uint32_t);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdDrawIndexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdDrawIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdDrawIndexedIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdDispatchIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdCopyBuffer(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit *, VkFilter);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *, uint32_t, const VkImageMemoryBarrier *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdClearAttachments(VkCommandBuffer, uint32_t, const VkClearAttachment *, uint32_t, const VkClearRect *);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdFillBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t);
VKAPI_ATTR void VKAPI_CALL cb_vkCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void *);

VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateFence(VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkWaitForFences(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkResetFences(VkDevice, uint32_t, const VkFence *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkGetFenceStatus(VkDevice, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *, VkSemaphore *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateEvent(VkDevice, const VkEventCreateInfo *, const VkAllocationCallbacks *, VkEvent *);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroyEvent(VkDevice, VkEvent, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL cb_vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR *);

VKAPI_ATTR void     VKAPI_CALL cb_vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkPresentModeKHR *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *, VkSwapchainKHR *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateCheeseBridgeSurfaceKHR(VkInstance, uint32_t, uint32_t, VkSurfaceKHR *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateXlibSurfaceKHR(VkInstance, const void *, const VkAllocationCallbacks *, VkSurfaceKHR *);
VKAPI_ATTR VkBool32 VKAPI_CALL cb_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice, uint32_t, void *, uintptr_t);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkCreateXcbSurfaceKHR(VkInstance, const void *, const VkAllocationCallbacks *, VkSurfaceKHR *);
VKAPI_ATTR VkBool32 VKAPI_CALL cb_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice, uint32_t, void *, uint32_t);
VKAPI_ATTR void     VKAPI_CALL cb_vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
VKAPI_ATTR VkResult VKAPI_CALL cb_vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *);

/* GetInstanceProcAddr / GetDeviceProcAddr themselves */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL cb_vkGetInstanceProcAddr(VkInstance, const char *);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL cb_vkGetDeviceProcAddr  (VkDevice,   const char *);

/* ---- ProcAddr table ----------------------------------------------------- */

struct cb_proc_entry {
    const char       *name;
    PFN_vkVoidFunction fn;
    bool              is_device;   /* should be returned by vkGetDeviceProcAddr */
};

#define E_INST(n) { #n, (PFN_vkVoidFunction)cb_##n, false }
#define E_DEV(n)  { #n, (PFN_vkVoidFunction)cb_##n, true  }

static const struct cb_proc_entry g_procs[] = {
    /* loader-visible global entry points */
    E_INST(vkGetInstanceProcAddr),
    E_INST(vkGetDeviceProcAddr),
    E_INST(vkCreateInstance),
    E_INST(vkDestroyInstance),
    E_INST(vkEnumeratePhysicalDevices),
    E_INST(vkEnumerateInstanceExtensionProperties),
    E_INST(vkEnumerateInstanceLayerProperties),
    E_INST(vkEnumerateInstanceVersion),

    E_INST(vkGetPhysicalDeviceProperties),
    E_INST(vkGetPhysicalDeviceFeatures),
    E_INST(vkGetPhysicalDeviceQueueFamilyProperties),
    E_INST(vkGetPhysicalDeviceMemoryProperties),
    E_INST(vkGetPhysicalDeviceFormatProperties),
    E_INST(vkGetPhysicalDeviceImageFormatProperties),
    E_INST(vkGetPhysicalDeviceSparseImageFormatProperties),
    E_INST(vkGetPhysicalDeviceFeatures2),
    E_INST(vkGetPhysicalDeviceFeatures2KHR),
    E_INST(vkGetPhysicalDeviceProperties2),
    E_INST(vkGetPhysicalDeviceProperties2KHR),
    E_INST(vkGetPhysicalDeviceFormatProperties2),
    E_INST(vkGetPhysicalDeviceFormatProperties2KHR),
    E_INST(vkGetPhysicalDeviceImageFormatProperties2),
    E_INST(vkGetPhysicalDeviceImageFormatProperties2KHR),
    E_INST(vkGetPhysicalDeviceQueueFamilyProperties2),
    E_INST(vkGetPhysicalDeviceQueueFamilyProperties2KHR),
    E_INST(vkGetPhysicalDeviceMemoryProperties2),
    E_INST(vkGetPhysicalDeviceMemoryProperties2KHR),
    E_INST(vkGetPhysicalDeviceSparseImageFormatProperties2),
    E_INST(vkGetPhysicalDeviceSparseImageFormatProperties2KHR),
    E_INST(vkGetPhysicalDeviceExternalBufferProperties),
    E_INST(vkGetPhysicalDeviceExternalBufferPropertiesKHR),
    E_INST(vkGetPhysicalDeviceExternalFenceProperties),
    E_INST(vkGetPhysicalDeviceExternalFencePropertiesKHR),
    E_INST(vkGetPhysicalDeviceExternalSemaphoreProperties),
    E_INST(vkGetPhysicalDeviceExternalSemaphorePropertiesKHR),
    E_INST(vkEnumerateDeviceExtensionProperties),
    E_INST(vkEnumerateDeviceLayerProperties),

    E_INST(vkCreateDevice),
    E_DEV (vkDestroyDevice),
    E_DEV (vkGetDeviceQueue),
    E_DEV (vkDeviceWaitIdle),
    E_DEV (vkQueueWaitIdle),

    E_DEV (vkAllocateMemory),  E_DEV (vkFreeMemory),
    E_DEV (vkMapMemory),       E_DEV (vkUnmapMemory),
    E_DEV (vkFlushMappedMemoryRanges),
    E_DEV (vkInvalidateMappedMemoryRanges),

    E_DEV (vkCreateBuffer),    E_DEV (vkDestroyBuffer),
    E_DEV (vkGetBufferMemoryRequirements),
    E_DEV (vkBindBufferMemory),
    E_DEV (vkCreateImage),     E_DEV (vkDestroyImage),
    E_DEV (vkGetImageMemoryRequirements),
    E_DEV (vkBindImageMemory),
    E_DEV (vkCreateImageView), E_DEV (vkDestroyImageView),
    E_DEV (vkCreateSampler),   E_DEV (vkDestroySampler),

    E_DEV (vkCreateShaderModule),    E_DEV (vkDestroyShaderModule),
    E_DEV (vkCreatePipelineLayout),  E_DEV (vkDestroyPipelineLayout),
    E_DEV (vkCreateGraphicsPipelines),
    E_DEV (vkCreateComputePipelines),
    E_DEV (vkDestroyPipeline),
    E_DEV (vkCreatePipelineCache),   E_DEV (vkDestroyPipelineCache),

    E_DEV (vkCreateRenderPass),  E_DEV (vkDestroyRenderPass),
    E_DEV (vkCreateFramebuffer), E_DEV (vkDestroyFramebuffer),

    E_DEV (vkCreateDescriptorSetLayout),  E_DEV (vkDestroyDescriptorSetLayout),
    E_DEV (vkCreateDescriptorPool),       E_DEV (vkDestroyDescriptorPool),
    E_DEV (vkAllocateDescriptorSets),     E_DEV (vkFreeDescriptorSets),
    E_DEV (vkUpdateDescriptorSets),

    E_DEV (vkCreateCommandPool),     E_DEV (vkDestroyCommandPool),
    E_DEV (vkResetCommandPool),
    E_DEV (vkAllocateCommandBuffers),E_DEV (vkFreeCommandBuffers),
    E_DEV (vkBeginCommandBuffer),    E_DEV (vkEndCommandBuffer),
    E_DEV (vkResetCommandBuffer),

    E_DEV (vkCmdBeginRenderPass),  E_DEV (vkCmdEndRenderPass),
    E_DEV (vkCmdNextSubpass),
    E_DEV (vkCmdBindPipeline),
    E_DEV (vkCmdBindVertexBuffers),E_DEV (vkCmdBindIndexBuffer),
    E_DEV (vkCmdBindDescriptorSets),E_DEV(vkCmdPushConstants),
    E_DEV (vkCmdSetViewport),      E_DEV (vkCmdSetScissor),
    E_DEV (vkCmdSetLineWidth),     E_DEV (vkCmdSetDepthBias),
    E_DEV (vkCmdSetBlendConstants),
    E_DEV (vkCmdSetStencilCompareMask),
    E_DEV (vkCmdSetStencilWriteMask),
    E_DEV (vkCmdSetStencilReference),
    E_DEV (vkCmdDraw),             E_DEV (vkCmdDrawIndexed),
    E_DEV (vkCmdDrawIndirect),     E_DEV (vkCmdDrawIndexedIndirect),
    E_DEV (vkCmdDispatch),         E_DEV (vkCmdDispatchIndirect),
    E_DEV (vkCmdCopyBuffer),       E_DEV (vkCmdCopyImage),
    E_DEV (vkCmdCopyBufferToImage),E_DEV (vkCmdCopyImageToBuffer),
    E_DEV (vkCmdBlitImage),        E_DEV (vkCmdPipelineBarrier),
    E_DEV (vkCmdClearColorImage),  E_DEV (vkCmdClearAttachments),
    E_DEV (vkCmdFillBuffer),       E_DEV (vkCmdUpdateBuffer),

    E_DEV (vkCreateFence),     E_DEV (vkDestroyFence),
    E_DEV (vkWaitForFences),   E_DEV (vkResetFences),
    E_DEV (vkGetFenceStatus),
    E_DEV (vkCreateSemaphore), E_DEV (vkDestroySemaphore),
    E_DEV (vkCreateEvent),     E_DEV (vkDestroyEvent),

    E_DEV (vkQueueSubmit),
    E_DEV (vkQueuePresentKHR),

    E_INST(vkDestroySurfaceKHR),
    E_INST(vkGetPhysicalDeviceSurfaceSupportKHR),
    E_INST(vkGetPhysicalDeviceSurfaceCapabilitiesKHR),
    E_INST(vkGetPhysicalDeviceSurfaceFormatsKHR),
    E_INST(vkGetPhysicalDeviceSurfacePresentModesKHR),
    E_INST(vkCreateXlibSurfaceKHR),
    E_INST(vkGetPhysicalDeviceXlibPresentationSupportKHR),
    E_INST(vkCreateXcbSurfaceKHR),
    E_INST(vkGetPhysicalDeviceXcbPresentationSupportKHR),

    E_DEV (vkCreateSwapchainKHR),    E_DEV (vkDestroySwapchainKHR),
    E_DEV (vkGetSwapchainImagesKHR), E_DEV (vkAcquireNextImageKHR),

    /* CheeseBridge-specific instance extension for getting a surface
     * backed by an NSWindow + CAMetalLayer on the macOS host. */
    E_INST(vkCreateCheeseBridgeSurfaceKHR),
};

static PFN_vkVoidFunction cb_lookup(const char *name, bool device_only) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_procs / sizeof g_procs[0]; ++i) {
        if (!strcmp(g_procs[i].name, name)) {
            if (device_only && !g_procs[i].is_device) return NULL;
            return g_procs[i].fn;
        }
    }
    return NULL;
}

PFN_vkVoidFunction cb_lookup_instance_proc(const char *n) { return cb_lookup(n, false); }
PFN_vkVoidFunction cb_lookup_device_proc  (const char *n) { return cb_lookup(n, true);  }

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
cb_vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    (void)instance;
    return cb_lookup(pName, false);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
cb_vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    (void)device;
    return cb_lookup(pName, true);
}

/* ---- Loader-facing exports --------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#  define CB_EXPORT __attribute__((visibility("default")))
#else
#  define CB_EXPORT
#endif

CB_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return cb_vkGetInstanceProcAddr(instance, pName);
}

CB_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName) {
    (void)instance;
    return cb_lookup_instance_proc(pName);
}

CB_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion) {
    /* We implement up to interface v5. Older loaders will downgrade us. */
    if (*pSupportedVersion > 5) *pSupportedVersion = 5;
    return VK_SUCCESS;
}
