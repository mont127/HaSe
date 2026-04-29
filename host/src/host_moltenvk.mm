#include "host.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

host_global_funcs_t g_vk;

static const char *cb_libmvk_candidates[] = {
    "libMoltenVK.dylib",
    "/usr/local/lib/libMoltenVK.dylib",
    "/opt/homebrew/lib/libMoltenVK.dylib",
    "@rpath/libMoltenVK.dylib",
};

VkResult host_load_moltenvk(void) {
    if (g_vk.GetInstanceProcAddr) return VK_SUCCESS;

    const char *override = getenv("CHEESEBRIDGE_MOLTENVK");
    if (override && *override) {
        g_vk.libmvk = dlopen(override, RTLD_LAZY | RTLD_LOCAL);
        if (!g_vk.libmvk) HE("dlopen(%s): %s", override, dlerror());
    }
    for (size_t i = 0; !g_vk.libmvk && i < sizeof cb_libmvk_candidates/sizeof *cb_libmvk_candidates; ++i) {
        g_vk.libmvk = dlopen(cb_libmvk_candidates[i], RTLD_LAZY | RTLD_LOCAL);
    }
    if (!g_vk.libmvk) {
        HE("could not locate libMoltenVK.dylib (set CHEESEBRIDGE_MOLTENVK)");
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    g_vk.GetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(g_vk.libmvk, "vkGetInstanceProcAddr");
    if (!g_vk.GetInstanceProcAddr) {
        HE("MoltenVK missing vkGetInstanceProcAddr: %s", dlerror());
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    g_vk.EnumerateInstanceVersion =
        (PFN_vkEnumerateInstanceVersion)g_vk.GetInstanceProcAddr(NULL,
            "vkEnumerateInstanceVersion");
    g_vk.EnumerateInstanceExtensionProperties =
        (PFN_vkEnumerateInstanceExtensionProperties)g_vk.GetInstanceProcAddr(NULL,
            "vkEnumerateInstanceExtensionProperties");
    g_vk.CreateInstance =
        (PFN_vkCreateInstance)g_vk.GetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!g_vk.CreateInstance) {
        HE("MoltenVK missing vkCreateInstance");
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    HI("loaded MoltenVK");
    return VK_SUCCESS;
}

#define LOAD_INST(N) out->N = (PFN_vk##N)g_vk.GetInstanceProcAddr(instance, "vk" #N)

void host_load_instance_funcs(VkInstance instance, host_instance_funcs_t *out) {
    memset(out, 0, sizeof *out);
    LOAD_INST(DestroyInstance);
    LOAD_INST(EnumeratePhysicalDevices);
    LOAD_INST(GetPhysicalDeviceProperties);
    LOAD_INST(GetPhysicalDeviceFeatures);
    LOAD_INST(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INST(GetPhysicalDeviceMemoryProperties);
    LOAD_INST(GetPhysicalDeviceFormatProperties);
    LOAD_INST(GetPhysicalDeviceImageFormatProperties);
    LOAD_INST(CreateDevice);
    LOAD_INST(GetDeviceProcAddr);
    LOAD_INST(EnumerateDeviceExtensionProperties);
    LOAD_INST(GetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INST(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INST(GetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INST(GetPhysicalDeviceSurfacePresentModesKHR);
    LOAD_INST(DestroySurfaceKHR);
    LOAD_INST(CreateMetalSurfaceEXT);
}

#define LOAD_DEV(N) out->N = (PFN_vk##N)ifn->GetDeviceProcAddr(device, "vk" #N)

void host_load_device_funcs(VkInstance instance, VkDevice device,
                            host_device_funcs_t *out) {
    (void)instance;
    /* We need GetDeviceProcAddr from the instance funcs to load device entries. */
    host_instance_funcs_t i; host_load_instance_funcs(instance, &i);
    host_instance_funcs_t *ifn = &i;
    memset(out, 0, sizeof *out);
    LOAD_DEV(DestroyDevice);
    LOAD_DEV(GetDeviceQueue);
    LOAD_DEV(DeviceWaitIdle);
    LOAD_DEV(QueueWaitIdle);
    LOAD_DEV(AllocateMemory); LOAD_DEV(FreeMemory);
    LOAD_DEV(MapMemory);      LOAD_DEV(UnmapMemory);
    LOAD_DEV(FlushMappedMemoryRanges);
    LOAD_DEV(InvalidateMappedMemoryRanges);
    LOAD_DEV(CreateBuffer); LOAD_DEV(DestroyBuffer);
    LOAD_DEV(GetBufferMemoryRequirements);
    LOAD_DEV(BindBufferMemory);
    LOAD_DEV(CreateImage);  LOAD_DEV(DestroyImage);
    LOAD_DEV(GetImageMemoryRequirements);
    LOAD_DEV(BindImageMemory);
    LOAD_DEV(CreateImageView); LOAD_DEV(DestroyImageView);
    LOAD_DEV(CreateSampler);   LOAD_DEV(DestroySampler);
    LOAD_DEV(CreateShaderModule);    LOAD_DEV(DestroyShaderModule);
    LOAD_DEV(CreatePipelineCache);   LOAD_DEV(DestroyPipelineCache);
    LOAD_DEV(CreatePipelineLayout);  LOAD_DEV(DestroyPipelineLayout);
    LOAD_DEV(CreateGraphicsPipelines);
    LOAD_DEV(CreateComputePipelines);
    LOAD_DEV(DestroyPipeline);
    LOAD_DEV(CreateRenderPass);  LOAD_DEV(DestroyRenderPass);
    LOAD_DEV(CreateFramebuffer); LOAD_DEV(DestroyFramebuffer);
    LOAD_DEV(CreateDescriptorSetLayout); LOAD_DEV(DestroyDescriptorSetLayout);
    LOAD_DEV(CreateDescriptorPool);      LOAD_DEV(DestroyDescriptorPool);
    LOAD_DEV(AllocateDescriptorSets);    LOAD_DEV(FreeDescriptorSets);
    LOAD_DEV(UpdateDescriptorSets);
    LOAD_DEV(CreateCommandPool); LOAD_DEV(DestroyCommandPool);
    LOAD_DEV(ResetCommandPool);
    LOAD_DEV(AllocateCommandBuffers); LOAD_DEV(FreeCommandBuffers);
    LOAD_DEV(BeginCommandBuffer);     LOAD_DEV(EndCommandBuffer);
    LOAD_DEV(ResetCommandBuffer);
    LOAD_DEV(CmdBeginRenderPass); LOAD_DEV(CmdEndRenderPass);
    LOAD_DEV(CmdNextSubpass);     LOAD_DEV(CmdBindPipeline);
    LOAD_DEV(CmdBindVertexBuffers); LOAD_DEV(CmdBindIndexBuffer);
    LOAD_DEV(CmdBindDescriptorSets); LOAD_DEV(CmdPushConstants);
    LOAD_DEV(CmdSetViewport); LOAD_DEV(CmdSetScissor);
    LOAD_DEV(CmdSetLineWidth); LOAD_DEV(CmdSetDepthBias);
    LOAD_DEV(CmdSetBlendConstants);
    LOAD_DEV(CmdSetStencilCompareMask);
    LOAD_DEV(CmdSetStencilWriteMask);
    LOAD_DEV(CmdSetStencilReference);
    LOAD_DEV(CmdDraw); LOAD_DEV(CmdDrawIndexed);
    LOAD_DEV(CmdDrawIndirect); LOAD_DEV(CmdDrawIndexedIndirect);
    LOAD_DEV(CmdDispatch); LOAD_DEV(CmdDispatchIndirect);
    LOAD_DEV(CmdCopyBuffer); LOAD_DEV(CmdCopyImage);
    LOAD_DEV(CmdCopyBufferToImage); LOAD_DEV(CmdCopyImageToBuffer);
    LOAD_DEV(CmdBlitImage); LOAD_DEV(CmdPipelineBarrier);
    LOAD_DEV(CmdClearColorImage); LOAD_DEV(CmdClearAttachments);
    LOAD_DEV(CmdFillBuffer); LOAD_DEV(CmdUpdateBuffer);
    LOAD_DEV(CreateFence); LOAD_DEV(DestroyFence);
    LOAD_DEV(WaitForFences); LOAD_DEV(ResetFences);
    LOAD_DEV(GetFenceStatus);
    LOAD_DEV(CreateSemaphore); LOAD_DEV(DestroySemaphore);
    LOAD_DEV(CreateEvent);     LOAD_DEV(DestroyEvent);
    LOAD_DEV(QueueSubmit);
    LOAD_DEV(CreateSwapchainKHR);  LOAD_DEV(DestroySwapchainKHR);
    LOAD_DEV(GetSwapchainImagesKHR); LOAD_DEV(AcquireNextImageKHR);
    LOAD_DEV(QueuePresentKHR);
}
