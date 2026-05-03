#include "icd.h"

/*
 * VK_EXT_debug_utils is commonly enabled by launchers, engines, and validation
 * wrappers even when validation is not present. CheeseBridge does not consume
 * labels or object names yet, but returning stable no-op entry points keeps
 * those applications on the normal Vulkan path.
 */

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkCreateDebugUtilsMessengerEXT(VkInstance instance,
                                  const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo,
                                  const VkAllocationCallbacks *pAllocator,
                                  VkDebugUtilsMessengerEXT *pMessenger) {
    (void)instance;
    (void)pCreateInfo;
    (void)pAllocator;
    if (!pMessenger) return VK_ERROR_INITIALIZATION_FAILED;
    *pMessenger = (VkDebugUtilsMessengerEXT)cb_next_id();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkDestroyDebugUtilsMessengerEXT(VkInstance instance,
                                   VkDebugUtilsMessengerEXT messenger,
                                   const VkAllocationCallbacks *pAllocator) {
    (void)instance;
    (void)messenger;
    (void)pAllocator;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkSubmitDebugUtilsMessageEXT(VkInstance instance,
                                VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                                const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData) {
    (void)instance;
    (void)messageSeverity;
    (void)messageTypes;
    (void)pCallbackData;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkSetDebugUtilsObjectNameEXT(VkDevice device,
                                const VkDebugUtilsObjectNameInfoEXT *pNameInfo) {
    (void)device;
    (void)pNameInfo;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
cb_vkSetDebugUtilsObjectTagEXT(VkDevice device,
                               const VkDebugUtilsObjectTagInfoEXT *pTagInfo) {
    (void)device;
    (void)pTagInfo;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkQueueBeginDebugUtilsLabelEXT(VkQueue queue,
                                  const VkDebugUtilsLabelEXT *pLabelInfo) {
    (void)queue;
    (void)pLabelInfo;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkQueueEndDebugUtilsLabelEXT(VkQueue queue) {
    (void)queue;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkQueueInsertDebugUtilsLabelEXT(VkQueue queue,
                                   const VkDebugUtilsLabelEXT *pLabelInfo) {
    (void)queue;
    (void)pLabelInfo;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdBeginDebugUtilsLabelEXT(VkCommandBuffer commandBuffer,
                                const VkDebugUtilsLabelEXT *pLabelInfo) {
    (void)commandBuffer;
    (void)pLabelInfo;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer commandBuffer) {
    (void)commandBuffer;
}

VKAPI_ATTR void VKAPI_CALL
cb_vkCmdInsertDebugUtilsLabelEXT(VkCommandBuffer commandBuffer,
                                 const VkDebugUtilsLabelEXT *pLabelInfo) {
    (void)commandBuffer;
    (void)pLabelInfo;
}
