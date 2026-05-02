/*
 * CheeseBridge Phase 4 demo: render a real triangle through the bridge.
 *
 * Drives the full graphics-pipeline path the Phase 3 clear demo skipped:
 *
 *   shader modules (precompiled SPIR-V embedded as C headers)
 *   render pass + framebuffer per swapchain image
 *   pipeline layout + graphics pipeline (no vertex buffer; gl_VertexIndex
 *     drives the 3 hard-coded positions in triangle.vert.glsl)
 *   per frame: barrier -> begin render pass -> bind pipeline ->
 *              set viewport/scissor -> draw -> end render pass ->
 *              barrier -> submit -> present
 *
 * Validates the Phase 4 host-side decoders for begin/end_render_pass,
 * bind_pipeline, set_viewport/scissor, and draw.
 *
 * Run inside a HaSe bottle after `hasectl install-icd <bottle>`:
 *
 *   CHEESEBRIDGE_HOST=tcp:host.lima.internal:43210 \
 *   /mnt/hase/vulkan/icd.d/triangle-demo
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "triangle_vert_spv.h"
#include "triangle_frag_spv.h"

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

static const uint32_t kFrameCount = 120;
static const uint32_t kWidth      = 640;
static const uint32_t kHeight     = 360;
static const VkFormat kColorFmt   = VK_FORMAT_B8G8R8A8_UNORM;

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
    return g_icd_gipa != NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    if (!load_icd()) { fprintf(stderr, "ICD load failed\n"); return 1; }

    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)g_icd_gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) { fprintf(stderr, "ICD has no vkCreateInstance\n"); return 1; }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "cheesebridge_triangle_demo",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, NULL, &instance));

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
    if (!cbCreateSurface) { fprintf(stderr, "missing CheeseBridge surface ext\n"); return 1; }

    /* ---- Pick physical device --------------------------------------------- */
    uint32_t pd_count = 0;
    CHECK(EnumeratePhysicalDevices(instance, &pd_count, NULL));
    if (!pd_count) { fprintf(stderr, "no physical devices\n"); return 1; }
    VkPhysicalDevice *pds = calloc(pd_count, sizeof *pds);
    CHECK(EnumeratePhysicalDevices(instance, &pd_count, pds));
    VkPhysicalDevice pd = pds[0];
    free(pds);
    VkPhysicalDeviceProperties props;
    GetPhysicalDeviceProperties(pd, &props);
    printf("Picked: %s\n", props.deviceName);

    uint32_t qf_count = 0;
    GetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, NULL);
    VkQueueFamilyProperties *qfs = calloc(qf_count ? qf_count : 1, sizeof *qfs);
    GetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs);
    uint32_t gfx_qf = 0;
    for (uint32_t i = 0; i < qf_count; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx_qf = i; break; }
    free(qfs);

    /* ---- Device + queue --------------------------------------------------- */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = gfx_qf, .queueCount = 1, .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
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
    LOAD_DEV(CmdBeginRenderPass);
    LOAD_DEV(CmdEndRenderPass);
    LOAD_DEV(CmdBindPipeline);
    LOAD_DEV(CmdSetViewport);
    LOAD_DEV(CmdSetScissor);
    LOAD_DEV(CmdDraw);
    LOAD_DEV(CreateSemaphore);
    LOAD_DEV(DestroySemaphore);
    LOAD_DEV(CreateSwapchainKHR);
    LOAD_DEV(DestroySwapchainKHR);
    LOAD_DEV(GetSwapchainImagesKHR);
    LOAD_DEV(AcquireNextImageKHR);
    LOAD_DEV(QueuePresentKHR);
    LOAD_DEV(CreateImageView);
    LOAD_DEV(DestroyImageView);
    LOAD_DEV(CreateRenderPass);
    LOAD_DEV(DestroyRenderPass);
    LOAD_DEV(CreateFramebuffer);
    LOAD_DEV(DestroyFramebuffer);
    LOAD_DEV(CreateShaderModule);
    LOAD_DEV(DestroyShaderModule);
    LOAD_DEV(CreatePipelineLayout);
    LOAD_DEV(DestroyPipelineLayout);
    LOAD_DEV(CreateGraphicsPipelines);
    LOAD_DEV(DestroyPipeline);
    LOAD_DEV(DestroyDevice);

    VkQueue queue;
    GetDeviceQueue(device, gfx_qf, 0, &queue);

    /* ---- Surface + swapchain --------------------------------------------- */
    VkSurfaceKHR surface;
    CHECK(cbCreateSurface(instance, kWidth, kHeight, &surface));

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = 2,
        .imageFormat = kColorFmt,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = { kWidth, kHeight },
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
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

    /* ---- Render pass ------------------------------------------------------ */
    VkAttachmentDescription att = {
        .format = kColorFmt,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &colorRef,
    };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 1, .pDependencies = &dep,
    };
    VkRenderPass rp;
    CHECK(CreateRenderPass(device, &rpi, NULL, &rp));

    /* ---- Image views + framebuffers --------------------------------------- */
    VkImageView *views = calloc(img_count, sizeof *views);
    VkFramebuffer *fbs = calloc(img_count, sizeof *fbs);
    for (uint32_t i = 0; i < img_count; ++i) {
        VkImageViewCreateInfo iv = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = imgs[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = kColorFmt,
            .subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
            },
        };
        CHECK(CreateImageView(device, &iv, NULL, &views[i]));
        VkFramebufferCreateInfo fi = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = rp,
            .attachmentCount = 1, .pAttachments = &views[i],
            .width = kWidth, .height = kHeight, .layers = 1,
        };
        CHECK(CreateFramebuffer(device, &fi, NULL, &fbs[i]));
    }

    /* ---- Shader modules + pipeline ---------------------------------------- */
    VkShaderModuleCreateInfo svi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof triangle_vert_spv,
        .pCode    = triangle_vert_spv,
    };
    VkShaderModule vsh;
    CHECK(CreateShaderModule(device, &svi, NULL, &vsh));

    VkShaderModuleCreateInfo sfi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof triangle_frag_spv,
        .pCode    = triangle_frag_spv,
    };
    VkShaderModule fsh;
    CHECK(CreateShaderModule(device, &sfi, NULL, &fsh));

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = vsh, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsh, .pName = "main" },
    };

    VkPipelineLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    VkPipelineLayout layout;
    CHECK(CreatePipelineLayout(device, &pli, NULL, &layout));

    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = { 0, 0, (float)kWidth, (float)kHeight, 0.0f, 1.0f };
    VkRect2D   sc = { { 0, 0 }, { kWidth, kHeight } };
    VkPipelineViewportStateCreateInfo vsi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount  = 1, .pScissors  = &sc,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                          VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba,
    };
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn_states,
    };
    VkGraphicsPipelineCreateInfo gpi = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vsi,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = layout,
        .renderPass = rp,
        .subpass = 0,
    };
    VkPipeline pipeline;
    CHECK(CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, NULL, &pipeline));

    /* ---- Command pool / buffer / sync ------------------------------------- */
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = gfx_qf,
    };
    VkCommandPool pool;
    CHECK(CreateCommandPool(device, &pci, NULL, &pool));

    VkCommandBufferAllocateInfo cba_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CHECK(AllocateCommandBuffers(device, &cba_alloc, &cmd));

    VkSemaphoreCreateInfo seminfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acq, ren;
    CHECK(CreateSemaphore(device, &seminfo, NULL, &acq));
    CHECK(CreateSemaphore(device, &seminfo, NULL, &ren));

    /* ---- Frame loop ------------------------------------------------------- */
    for (uint32_t f = 0; f < kFrameCount; ++f) {
        uint32_t idx = 0;
        VkResult ar = AcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                          acq, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "acquire failed: %d\n", ar); break;
        }

        CHECK(ResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        CHECK(BeginCommandBuffer(cmd, &bi));

        float t = (float)f / kFrameCount;
        VkClearValue cv = { .color = { .float32 = { 0.05f, 0.05f, 0.10f + 0.5f*t, 1.0f } } };
        VkRenderPassBeginInfo rbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = rp,
            .framebuffer = fbs[idx],
            .renderArea = { { 0, 0 }, { kWidth, kHeight } },
            .clearValueCount = 1, .pClearValues = &cv,
        };
        CmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        CmdSetViewport(cmd, 0, 1, &vp);
        CmdSetScissor (cmd, 0, 1, &sc);
        CmdDraw(cmd, 3, 1, 0, 0);
        CmdEndRenderPass(cmd);

        CHECK(EndCommandBuffer(cmd));

        VkPipelineStageFlags wait_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &acq,
            .pWaitDstStageMask = &wait_mask,
            .commandBufferCount = 1, .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &ren,
        };
        CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE));

        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &ren,
            .swapchainCount = 1, .pSwapchains = &swapchain,
            .pImageIndices = &idx,
        };
        VkResult pr = QueuePresentKHR(queue, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "present failed: %d\n", pr); break;
        }
        QueueWaitIdle(queue);

        struct timespec ts = { 0, 16 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (f % 30 == 0) printf("frame %u\n", f);
    }

    DeviceWaitIdle(device);
    DestroyPipeline(device, pipeline, NULL);
    DestroyPipelineLayout(device, layout, NULL);
    DestroyShaderModule(device, vsh, NULL);
    DestroyShaderModule(device, fsh, NULL);
    for (uint32_t i = 0; i < img_count; ++i) {
        DestroyFramebuffer(device, fbs[i], NULL);
        DestroyImageView (device, views[i], NULL);
    }
    free(views); free(fbs); free(imgs);
    DestroyRenderPass(device, rp, NULL);
    DestroySemaphore(device, acq, NULL);
    DestroySemaphore(device, ren, NULL);
    DestroyCommandPool(device, pool, NULL);
    DestroySwapchainKHR(device, swapchain, NULL);
    DestroySurfaceKHR(instance, surface, NULL);
    DestroyDevice(device, NULL);
    DestroyInstance(instance, NULL);
    return 0;
}
