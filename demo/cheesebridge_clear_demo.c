/*
 * CheeseBridge Phase 3 end-to-end demo.
 *
 * Drives the CheeseBridge ICD all the way to a clear-color frame on the
 * macOS host: instance -> device -> surface -> swapchain -> N frames of
 * acquire/clear/present.
 *
 * The Vulkan loader is bypassed deliberately. The CheeseBridge surface
 * extension is non-standard, so the loader rejects or trampolines its
 * function pointer in ways that break the dispatchable-handle cast inside
 * the ICD. Instead we dlopen the ICD shared object and dispatch every
 * Vulkan call through its vk_icdGetInstanceProcAddr — exactly what a
 * Vulkan loader does, just inline.
 *
 *   CHEESEBRIDGE_ICD_PATH=...libCheeseBridge_icd.so   (defaulted)
 *   CHEESEBRIDGE_STUB=0
 *   CHEESEBRIDGE_HOST=tcp:host.lima.internal:43210
 *   ./cheesebridge_clear_demo
 *
 * Expected: a window pops up on the macOS host and cycles colored frames.
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef VkResult (VKAPI_PTR *PFN_cbCreateSurface)(VkInstance, uint32_t, uint32_t, VkSurfaceKHR *);

#define CHECK(expr) do {                                                        \
    VkResult _r = (expr);                                                       \
    if (_r != VK_SUCCESS) {                                                     \
        fprintf(stderr, "%s failed: %d\n", #expr, _r);                          \
        return 1;                                                               \
    }                                                                           \
} while (0)

static const uint32_t kFrameCount = 60;
static const uint32_t kWidth      = 640;
static const uint32_t kHeight     = 360;

static PFN_vkGetInstanceProcAddr g_icd_gipa;
static PFN_vkGetDeviceProcAddr   g_icd_gdpa;

#define LOAD_INST(NAME) PFN_vk##NAME NAME = (PFN_vk##NAME)g_icd_gipa(instance, "vk" #NAME)
#define LOAD_DEV(NAME)  PFN_vk##NAME NAME = (PFN_vk##NAME)g_icd_gdpa(device,   "vk" #NAME)

static int load_icd(void) {
    const char *icd_path = getenv("CHEESEBRIDGE_ICD_PATH");
    if (!icd_path || !*icd_path) icd_path = "libCheeseBridge_icd.so";
    void *lib = dlopen(icd_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", icd_path, dlerror());
        return 0;
    }
    g_icd_gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!g_icd_gipa)
        g_icd_gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!g_icd_gipa) {
        fprintf(stderr, "ICD missing GetInstanceProcAddr\n");
        return 0;
    }
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    if (!load_icd()) return 1;

    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)g_icd_gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) { fprintf(stderr, "ICD has no vkCreateInstance\n"); return 1; }

    /* ---- Instance --------------------------------------------------------- */
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "cheesebridge_clear_demo",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, NULL, &instance));

    /* From here on we resolve everything through the ICD's instance gipa. */
    LOAD_INST(EnumeratePhysicalDevices);
    LOAD_INST(GetPhysicalDeviceProperties);
    LOAD_INST(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INST(CreateDevice);
    LOAD_INST(GetDeviceProcAddr);
    LOAD_INST(DestroySurfaceKHR);
    LOAD_INST(DestroyInstance);

    g_icd_gdpa = GetDeviceProcAddr;

    PFN_cbCreateSurface cbCreateSurface = (PFN_cbCreateSurface)
        g_icd_gipa(instance, "vkCreateCheeseBridgeSurfaceKHR");
    if (!cbCreateSurface) {
        fprintf(stderr, "ICD missing vkCreateCheeseBridgeSurfaceKHR\n");
        return 1;
    }

    /* ---- Physical device + queue family ----------------------------------- */
    uint32_t pd_count = 0;
    CHECK(EnumeratePhysicalDevices(instance, &pd_count, NULL));
    if (!pd_count) { fprintf(stderr, "no physical devices\n"); return 1; }
    VkPhysicalDevice *pds = calloc(pd_count, sizeof *pds);
    CHECK(EnumeratePhysicalDevices(instance, &pd_count, pds));
    VkPhysicalDevice pd = pds[0];
    free(pds);

    VkPhysicalDeviceProperties props;
    GetPhysicalDeviceProperties(pd, &props);
    printf("Picked: %s (driverVersion=%u)\n", props.deviceName, props.driverVersion);

    uint32_t qf_count = 0;
    GetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, NULL);
    VkQueueFamilyProperties *qfs = calloc(qf_count ? qf_count : 1, sizeof *qfs);
    GetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs);
    uint32_t gfx_qf = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx_qf = i; break; }
    free(qfs);
    if (gfx_qf == UINT32_MAX) gfx_qf = 0;

    /* ---- Device + queue --------------------------------------------------- */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = gfx_qf,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice device;
    CHECK(CreateDevice(pd, &dci, NULL, &device));

    LOAD_DEV(GetDeviceQueue);
    LOAD_DEV(DeviceWaitIdle);
    LOAD_DEV(QueueWaitIdle);
    LOAD_DEV(QueueSubmit);
    LOAD_DEV(CreateCommandPool);
    LOAD_DEV(DestroyCommandPool);
    LOAD_DEV(AllocateCommandBuffers);
    LOAD_DEV(BeginCommandBuffer);
    LOAD_DEV(EndCommandBuffer);
    LOAD_DEV(ResetCommandBuffer);
    LOAD_DEV(CmdPipelineBarrier);
    LOAD_DEV(CmdClearColorImage);
    LOAD_DEV(CreateSemaphore);
    LOAD_DEV(DestroySemaphore);
    LOAD_DEV(CreateSwapchainKHR);
    LOAD_DEV(DestroySwapchainKHR);
    LOAD_DEV(GetSwapchainImagesKHR);
    LOAD_DEV(AcquireNextImageKHR);
    LOAD_DEV(QueuePresentKHR);
    LOAD_DEV(DestroyDevice);

    VkQueue queue;
    GetDeviceQueue(device, gfx_qf, 0, &queue);

    /* ---- Surface (CheeseBridge extension) --------------------------------- */
    VkSurfaceKHR surface;
    CHECK(cbCreateSurface(instance, kWidth, kHeight, &surface));

    /* ---- Swapchain -------------------------------------------------------- */
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = 2,
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = { kWidth, kHeight },
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain;
    CHECK(CreateSwapchainKHR(device, &sci, NULL, &swapchain));

    uint32_t img_count = 0;
    CHECK(GetSwapchainImagesKHR(device, swapchain, &img_count, NULL));
    VkImage *imgs = calloc(img_count, sizeof *imgs);
    CHECK(GetSwapchainImagesKHR(device, swapchain, &img_count, imgs));
    printf("Swapchain images: %u\n", img_count);

    /* ---- Command pool / buffer ------------------------------------------- */
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = gfx_qf,
    };
    VkCommandPool pool;
    CHECK(CreateCommandPool(device, &pci, NULL, &pool));

    VkCommandBufferAllocateInfo cba = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CHECK(AllocateCommandBuffers(device, &cba, &cmd));

    VkSemaphoreCreateInfo seminfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquire_sem, render_sem;
    CHECK(CreateSemaphore(device, &seminfo, NULL, &acquire_sem));
    CHECK(CreateSemaphore(device, &seminfo, NULL, &render_sem));

    /* ---- Frame loop ------------------------------------------------------- */
    for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
        uint32_t idx = 0;
        VkResult ar = AcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                          acquire_sem, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "acquire failed: %d\n", ar);
            break;
        }
        VkImage img = imgs[idx];

        CHECK(ResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        CHECK(BeginCommandBuffer(cmd, &bi));

        VkImageSubresourceRange whole = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        };

        VkImageMemoryBarrier to_dst = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img,
            .subresourceRange = whole,
        };
        CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &to_dst);

        float t = (float)frame / kFrameCount;
        VkClearColorValue color = { .float32 = { t, 0.4f, 1.0f - t, 1.0f } };
        CmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &color, 1, &whole);

        VkImageMemoryBarrier to_present = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = 0,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img,
            .subresourceRange = whole,
        };
        CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, NULL, 0, NULL, 1, &to_present);

        CHECK(EndCommandBuffer(cmd));

        VkPipelineStageFlags wait_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = &acquire_sem,
            .pWaitDstStageMask  = &wait_mask,
            .commandBufferCount = 1,
            .pCommandBuffers    = &cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores    = &render_sem,
        };
        CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE));

        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = &render_sem,
            .swapchainCount     = 1,
            .pSwapchains        = &swapchain,
            .pImageIndices      = &idx,
        };
        VkResult pr = QueuePresentKHR(queue, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "present failed: %d\n", pr);
            break;
        }

        QueueWaitIdle(queue);

        struct timespec ts = { 0, 33 * 1000 * 1000 }; /* ~30 fps */
        nanosleep(&ts, NULL);
        printf("frame %u idx=%u t=%.2f\n", frame, idx, t);
    }

    DeviceWaitIdle(device);
    DestroySemaphore(device, acquire_sem, NULL);
    DestroySemaphore(device, render_sem, NULL);
    DestroyCommandPool(device, pool, NULL);
    free(imgs);
    DestroySwapchainKHR(device, swapchain, NULL);
    DestroySurfaceKHR(instance, surface, NULL);
    DestroyDevice(device, NULL);
    DestroyInstance(instance, NULL);
    return 0;
}
