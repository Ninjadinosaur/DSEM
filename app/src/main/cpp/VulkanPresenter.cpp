#include "VulkanPresenter.h"
#include "LogBuffer.h"
#include "vk/VkStream.h"

#include <android/native_window.h>
#include <swappy/swappyVk.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ds13r
{

namespace
{
// SPIR-V, compiled from shaders/present.* by glslc at build time.
const uint32_t kPresentVert[] = {
#include "present.vert.inc"
};
const uint32_t kPresentFrag[] = {
#include "present.frag.inc"
};

// Must match shaders/present_common.glsl.
struct PushConstants
{
    float rect[4];
    float preRotation[4];
    float texSize[2];
    float outSize[2];
    float layer;
    float opacity;
    int32_t rotation;
    int32_t flipY;
    int32_t filterMode;
    int32_t colorCorrect;
    int32_t swapRB;
};
static_assert(sizeof(PushConstants) == 76, "push constant layout");
}

VulkanPresenter::VulkanPresenter(VulkanContext& vk, JavaVM* vm, jobject activity) : vk(vk), vm(vm), activity(activity)
{
}

VulkanPresenter::~VulkanPresenter()
{
    Shutdown();
}

bool VulkanPresenter::Init()
{
    if (initialized) return true;
    if (!vk.Ready()) return false;
    VkDevice dev = vk.Device();

    // The swapchain format is decided per surface, but every Android device offers RGBA8.
    surfaceFormat = VK_FORMAT_R8G8B8A8_UNORM;

    VkSamplerCreateInfo si {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    vkCreateSampler(dev, &si, nullptr, &sampler);

    VkDescriptorSetLayoutBinding b {};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &b;
    vkCreateDescriptorSetLayout(dev, &dli, nullptr, &setLayout);

    VkDescriptorPoolSize ps {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight};
    VkDescriptorPoolCreateInfo dpi {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kFramesInFlight;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    vkCreateDescriptorPool(dev, &dpi, nullptr, &descriptorPool);

    for (Frame& f : frames)
    {
        VkCommandPoolCreateInfo cpi {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cpi.queueFamilyIndex = vk.QueueFamily();
        vkCreateCommandPool(dev, &cpi, nullptr, &f.pool);
        VkCommandBufferAllocateInfo ai {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &ai, &f.cmd);
        VkFenceCreateInfo fi {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev, &fi, nullptr, &f.fence);
        VkSemaphoreCreateInfo sci {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev, &sci, nullptr, &f.acquired);
        VkDescriptorSetAllocateInfo dai {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = descriptorPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &setLayout;
        vkAllocateDescriptorSets(dev, &dai, &f.set);
    }

    if (!CreatePipeline()) return false;
    initialized = true;
    LOGI("Vulkan presenter ready");
    return true;
}

bool VulkanPresenter::CreatePipeline()
{
    VkDevice dev = vk.Device();

    VkAttachmentDescription color {};
    color.format = surfaceFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &color;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    if (vkCreateRenderPass(dev, &rpi, nullptr, &renderPass) != VK_SUCCESS) return false;

    VkPushConstantRange pcr {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(dev, &pli, nullptr, &pipelineLayout);

    VkShaderModule vs = vk.CreateShaderModule(kPresentVert, sizeof(kPresentVert));
    VkShaderModule fs = vk.CreateShaderModule(kPresentFrag, sizeof(kPresentFrag));
    VkPipelineShaderStageCreateInfo stages[2] {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};

    VkPipelineVertexInputStateCreateInfo vi {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend {};
    blend.blendEnable = VK_TRUE; // picture-in-picture screens can be translucent
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gpi {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &ds;
    gpi.layout = pipelineLayout;
    gpi.renderPass = renderPass;
    VkResult r = vkCreateGraphicsPipelines(dev, vk.PipelineCache(), 1, &gpi, nullptr, &pipeline);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    if (r != VK_SUCCESS)
    {
        LOGE("Vulkan: presenter pipeline failed (%s)", VkResultName(r));
        return false;
    }
    vk.SavePipelineCache();
    return true;
}

void VulkanPresenter::Shutdown()
{
    if (!initialized) return;
    VkDevice dev = vk.Device();
    vk.DeviceWaitIdle();
    ReleaseWindow();
    for (Frame& f : frames)
    {
        if (f.externalView) vkDestroyImageView(dev, f.externalView, nullptr);
        f.staging.Destroy(vk);
        f.texture.Destroy(vk);
        if (f.fence) vkDestroyFence(dev, f.fence, nullptr);
        if (f.acquired) vkDestroySemaphore(dev, f.acquired, nullptr);
        if (f.pool) vkDestroyCommandPool(dev, f.pool, nullptr);
        f = Frame {};
    }
    if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    if (renderPass) vkDestroyRenderPass(dev, renderPass, nullptr);
    if (descriptorPool) vkDestroyDescriptorPool(dev, descriptorPool, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
    if (sampler) vkDestroySampler(dev, sampler, nullptr);
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    renderPass = VK_NULL_HANDLE;
    descriptorPool = VK_NULL_HANDLE;
    setLayout = VK_NULL_HANDLE;
    sampler = VK_NULL_HANDLE;
    initialized = false;
}

void VulkanPresenter::SetWindow(ANativeWindow* w)
{
    ReleaseWindow();
    if (!w || !initialized) return;
    window = w;
    ANativeWindow_acquire(window);

    VkAndroidSurfaceCreateInfoKHR sci {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    sci.window = window;
    VkResult r = vkCreateAndroidSurfaceKHR(vk.Instance(), &sci, nullptr, &surface);
    if (r != VK_SUCCESS)
    {
        LOGE("Vulkan: surface creation failed (%s)", VkResultName(r));
        ANativeWindow_release(window);
        window = nullptr;
        return;
    }
    if (!CreateSwapchain()) ReleaseWindow();
}

bool VulkanPresenter::CreateSwapchain()
{
    VkPhysicalDevice pd = vk.PhysicalDevice();
    VkDevice dev = vk.Device();

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(pd, vk.QueueFamily(), surface, &supported);
    if (!supported)
    {
        LOGE("Vulkan: queue can't present to this surface");
        return false;
    }

    VkSurfaceCapabilitiesKHR caps {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);

    // Pre-rotation (developer.android.com/games/optimize/vulkan-prerotation): take the
    // display's current transform, build the swapchain in the panel's native orientation,
    // and rotate our drawing to match.
    transform = caps.currentTransform;
    extent = caps.currentExtent;
    bool sideways = transform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    if (sideways) std::swap(extent.width, extent.height);
    logicalWidth = (int)(sideways ? extent.height : extent.width);
    logicalHeight = (int)(sideways ? extent.width : extent.height);

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen = f;
    if (chosen.format != surfaceFormat)
    {
        LOGE("Vulkan: surface doesn't offer RGBA8 (format %d)", chosen.format);
        return false;
    }

    uint32_t imageCount = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    VkSwapchainCreateInfoKHR ci {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = transform;
    ci.compositeAlpha = alpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    VkResult r = vkCreateSwapchainKHR(dev, &ci, nullptr, &swapchain);
    if (r != VK_SUCCESS)
    {
        LOGE("Vulkan: swapchain creation failed (%s)", VkResultName(r));
        swapchain = VK_NULL_HANDLE;
        return false;
    }

    vkGetSwapchainImagesKHR(dev, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(dev, swapchain, &count, images.data());
    imageViews.resize(count);
    framebuffers.resize(count);
    renderDone.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        VkImageViewCreateInfo vi {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = chosen.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(dev, &vi, nullptr, &imageViews[i]);
        VkFramebufferCreateInfo fi {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = renderPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &imageViews[i];
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = 1;
        vkCreateFramebuffer(dev, &fi, nullptr, &framebuffers[i]);
        VkSemaphoreCreateInfo sci {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev, &sci, nullptr, &renderDone[i]);
    }

    // Frame pacing against the display's vsync.
    JNIEnv* env = nullptr;
    swappyReady = false;
    if (vm && activity && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
    {
        uint64_t refreshNs = 0;
        swappyReady = SwappyVk_initAndGetRefreshCycleDuration(env, activity, pd, dev, swapchain, &refreshNs);
        if (swappyReady)
        {
            SwappyVk_setWindow(dev, swapchain, window);
            SwappyVk_setAutoSwapInterval(false);
            SwappyVk_setAutoPipelineMode(false);
        }
    }
    needRecreate = false;
    SetTargetRefreshRate(targetHz);
    LOGI("Vulkan: swapchain %ux%u (%u images), transform %d, frame pacing %s", extent.width, extent.height, count,
         (int)transform, swappyReady ? "via SwappyVk" : "FIFO only");
    return true;
}

void VulkanPresenter::DestroySwapchain()
{
    VkDevice dev = vk.Device();
    if (!swapchain) return;
    vk.DeviceWaitIdle();
    for (VkFramebuffer fb : framebuffers) vkDestroyFramebuffer(dev, fb, nullptr);
    for (VkImageView v : imageViews) vkDestroyImageView(dev, v, nullptr);
    for (VkSemaphore s : renderDone) vkDestroySemaphore(dev, s, nullptr);
    framebuffers.clear();
    imageViews.clear();
    renderDone.clear();
    images.clear();
    if (swappyReady) SwappyVk_destroySwapchain(dev, swapchain);
    vkDestroySwapchainKHR(dev, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
    swappyReady = false;
}

void VulkanPresenter::ReleaseWindow()
{
    DestroySwapchain();
    if (surface)
    {
        vkDestroySurfaceKHR(vk.Instance(), surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
    if (window)
    {
        ANativeWindow_release(window);
        window = nullptr;
    }
}

void VulkanPresenter::SetLayout(const PresentLayout& l)
{
    std::lock_guard<std::mutex> lock(layoutLock);
    layout = l;
}

void VulkanPresenter::SetSettings(const PresentSettings& s)
{
    std::lock_guard<std::mutex> lock(layoutLock);
    settings = s;
}

void VulkanPresenter::UploadSoftwareFrame(const void* const* screens, int count, int width, int height, bool bgra)
{
    size_t perScreen = (size_t)width * height;
    softPixels.resize(perScreen * count);
    for (int i = 0; i < count; i++) memcpy(softPixels.data() + perScreen * i, screens[i], perScreen * 4);
    softWidth = width;
    softHeight = height;
    softLayers = count;
    softBgra = bgra;
    softVersion++;
    haveFrame = true;
    useExternal = false;
    useExternalImage = false;
}

void VulkanPresenter::SetExternalFrame(void* texture, int width, int height, int layers)
{
    extTexture = texture;
    extWidth = width;
    extHeight = height;
    extLayers = layers;
    useExternal = true;
    useExternalImage = false;
    haveFrame = true;
}

void VulkanPresenter::SetExternalImage(VkImage image, VkFormat format, int width, int height)
{
    extImage = image;
    extFormat = format;
    extWidth = width;
    extHeight = height;
    extLayers = 1;
    useExternalImage = true;
    useExternal = false;
    haveFrame = true;
}

void VulkanPresenter::DropExternalImage()
{
    if (!useExternalImage) return;
    extImage = VK_NULL_HANDLE;
    useExternalImage = false;
    haveFrame = false;
}

void VulkanPresenter::DropExternalFrames()
{
    DropExternalImage();
    if (useExternal)
    {
        extTexture = nullptr;
        useExternal = false;
        haveFrame = false;
    }
    // A new object can reuse a destroyed view's handle value: force every slot to rewrite its
    // descriptor next time rather than trusting the cached handle.
    for (Frame& f : frames) f.boundView = VK_NULL_HANDLE;
}

void VulkanPresenter::WaitSlot(int slot)
{
    if (!initialized || slot < 0 || slot >= kFramesInFlight) return;
    vkWaitForFences(vk.Device(), 1, &frames[slot].fence, VK_TRUE, UINT64_MAX);
}

void VulkanPresenter::BindView(Frame& f, VkImageView view)
{
    if (f.boundView == view) return;
    VkDescriptorImageInfo ii {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = f.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk.Device(), 1, &w, 0, nullptr);
    f.boundView = view;
}

// Makes sure this frame slot's texture holds the latest software frame; records the upload.
bool VulkanPresenter::PrepareFrameTexture(Frame& f)
{
    if (!haveFrame) return false;
    VkDeviceSize bytes = (VkDeviceSize)softPixels.size() * 4;
    bool resized = f.texture.width != (uint32_t)softWidth || f.texture.height != (uint32_t)softHeight ||
                   f.texture.layers != (uint32_t)softLayers;
    if (resized)
    {
        f.texture.Create(vk, softWidth, softHeight, softLayers, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        f.textureReady = false;
        f.textureVersion = 0;
        f.boundView = VK_NULL_HANDLE;
    }
    if (f.staging.size < bytes)
        f.staging.Create(vk, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (!f.texture.image || !f.staging.mapped) return false;

    if (f.textureVersion != softVersion)
    {
        memcpy(f.staging.mapped, softPixels.data(), bytes);
        vmaFlushAllocation(vk.Allocator(), f.staging.allocation, 0, bytes);
        TransitionImage(f.cmd, f.texture.image,
                        f.textureReady ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, (uint32_t)softLayers};
        copy.imageExtent = {(uint32_t)softWidth, (uint32_t)softHeight, 1};
        vkCmdCopyBufferToImage(f.cmd, f.staging.buffer, f.texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        TransitionImage(f.cmd, f.texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        f.textureVersion = softVersion;
        f.textureReady = true;
    }
    if (f.textureReady) BindView(f, f.texture.view);
    return f.textureReady;
}

bool VulkanPresenter::Present()
{
    if (!swapchain) return false;
    if (needRecreate)
    {
        DestroySwapchain();
        if (!CreateSwapchain()) return false;
    }

    VkDevice dev = vk.Device();
    Frame& f = frames[frameIndex];
    vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        needRecreate = true;
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        LOGW("Vulkan: acquire failed (%s)", VkResultName(r));
        return false;
    }
    vkResetFences(dev, 1, &f.fence);

    PresentLayout l;
    PresentSettings s;
    {
        std::lock_guard<std::mutex> lock(layoutLock);
        l = layout;
        s = settings;
    }

    vkResetCommandPool(dev, f.pool, 0);
    VkCommandBufferBeginInfo bi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);

    bool drawScreens;
    int texW, texH, texLayers;
    bool swapRB;
    // This slot's GPU work is finished (fence waited above): its per-frame view can go.
    if (f.externalView)
    {
        vkDestroyImageView(dev, f.externalView, nullptr);
        f.externalView = VK_NULL_HANDLE;
        f.boundView = VK_NULL_HANDLE;
    }
    if (useExternalImage && extImage)
    {
        VkImageViewCreateInfo vi {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = extImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format = extFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(dev, &vi, nullptr, &f.externalView);
        BindView(f, f.externalView);
        drawScreens = f.externalView != VK_NULL_HANDLE;
        texW = extWidth;
        texH = extHeight;
        texLayers = 1;
        swapRB = false;
    }
    else if (useExternal && extTexture)
    {
        BindView(f, static_cast<vk::Texture*>(extTexture)->view);
        drawScreens = true;
        texW = extWidth;
        texH = extHeight;
        texLayers = extLayers;
        swapRB = false;
    }
    else
    {
        drawScreens = PrepareFrameTexture(f);
        texW = softWidth;
        texH = softHeight;
        texLayers = softLayers;
        swapRB = softBgra;
    }

    VkClearValue clear {};
    clear.color.float32[0] = ((s.backgroundArgb >> 16) & 0xFF) / 255.0f;
    clear.color.float32[1] = ((s.backgroundArgb >> 8) & 0xFF) / 255.0f;
    clear.color.float32[2] = (s.backgroundArgb & 0xFF) / 255.0f;
    clear.color.float32[3] = ((s.backgroundArgb >> 24) & 0xFF) / 255.0f;
    VkRenderPassBeginInfo rbi {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = renderPass;
    rbi.framebuffer = framebuffers[imageIndex];
    rbi.renderArea.extent = extent;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(f.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    if (drawScreens && logicalWidth > 0 && logicalHeight > 0)
    {
        VkViewport viewport {0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f};
        VkRect2D scissor {{0, 0}, extent};
        vkCmdSetViewport(f.cmd, 0, 1, &viewport);
        vkCmdSetScissor(f.cmd, 0, 1, &scissor);
        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &f.set, 0, nullptr);

        // Rotation that takes the app's (logical) orientation to the panel's native one.
        float angle = 0.0f;
        if (transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) angle = 90.0f;
        else if (transform & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) angle = 180.0f;
        else if (transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) angle = 270.0f;
        float rad = angle * 3.14159265f / 180.0f;
        float c = std::round(std::cos(rad)), sn = std::round(std::sin(rad));

        for (const ScreenQuad& q : l.quads)
        {
            if (q.w <= 0 || q.h <= 0 || q.opacity <= 0.0f || q.screen >= texLayers) continue;
            int rot = ((q.rotation / 90) % 4 + 4) % 4;
            bool sidewaysQuad = rot == 1 || rot == 3;
            PushConstants pc {};
            // Vulkan NDC: y points down, so top-left of the surface is (-1, -1).
            pc.rect[0] = q.x / logicalWidth * 2.0f - 1.0f;
            pc.rect[1] = q.y / logicalHeight * 2.0f - 1.0f;
            pc.rect[2] = (q.x + q.w) / logicalWidth * 2.0f - 1.0f;
            pc.rect[3] = (q.y + q.h) / logicalHeight * 2.0f - 1.0f;
            pc.preRotation[0] = c;
            pc.preRotation[1] = sn;
            pc.preRotation[2] = -sn;
            pc.preRotation[3] = c;
            pc.texSize[0] = (float)texW;
            pc.texSize[1] = (float)texH;
            pc.outSize[0] = sidewaysQuad ? q.h : q.w;
            pc.outSize[1] = sidewaysQuad ? q.w : q.h;
            pc.layer = (float)q.screen;
            pc.opacity = q.opacity;
            pc.rotation = rot;
            pc.flipY = 0;
            pc.filterMode = (int)s.filter;
            pc.colorCorrect = s.colorCorrection ? 1 : 0;
            pc.swapRB = swapRB ? 1 : 0;
            vkCmdPushConstants(f.cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(pc), &pc);
            vkCmdDraw(f.cmd, 4, 1, 0, 0);
        }
    }
    vkCmdEndRenderPass(f.cmd);
    vkEndCommandBuffer(f.cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone[imageIndex];
    r = vk.Submit(si, f.fence);
    if (r != VK_SUCCESS)
    {
        LOGE("Vulkan: submit failed (%s)", VkResultName(r));
        return false;
    }

    VkPresentInfoKHR pi {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &imageIndex;
    vk.LockQueue();
    r = swappyReady ? SwappyVk_queuePresent(vk.Queue(), &pi) : vkQueuePresentKHR(vk.Queue(), &pi);
    vk.UnlockQueue();
    frameIndex = (frameIndex + 1) % kFramesInFlight;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        // e.g. the display rotated: rebuild with the new transform before the next frame.
        needRecreate = true;
        return r == VK_SUBOPTIMAL_KHR;
    }
    if (r != VK_SUCCESS)
    {
        LOGW("Vulkan: present failed (%s)", VkResultName(r));
        return false;
    }
    return true;
}

void VulkanPresenter::SetTargetRefreshRate(float hz)
{
    targetHz = hz;
    if (!window) return;
    // Same policy as the GL presenter: a fixed 60 Hz vote in play, none while paused.
    ANativeWindow_setFrameRate(window, hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
    if (swappyReady && hz > 0.0f) SwappyVk_setSwapIntervalNS(vk.Device(), swapchain, (uint64_t)(1e9 / hz));
}

bool VulkanPresenter::ReadScreen(int screen, std::vector<uint32_t>& out, int& width, int& height)
{
    if (useExternalImage && extImage)
    {
        // The 3DS engine's output (created with TRANSFER_SRC for libretro frontends).
        if (screen != 0) return false;
        width = extWidth;
        height = extHeight;
        size_t bytes = (size_t)width * height * 4;
        VkBufferResource readback;
        if (!readback.Create(vk, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true)) return false;
        bool ok = vk.RunOnce([&](VkCommandBuffer cmd) {
            TransitionImage(cmd, extImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, 1);
            VkBufferImageCopy c {};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
            vkCmdCopyImageToBuffer(cmd, extImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &c);
            TransitionImage(cmd, extImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 1);
        });
        if (ok)
        {
            vmaInvalidateAllocation(vk.Allocator(), readback.allocation, 0, bytes);
            out.resize((size_t)width * height);
            memcpy(out.data(), readback.mapped, bytes);
            bool bgra = extFormat == VK_FORMAT_B8G8R8A8_UNORM || extFormat == VK_FORMAT_B8G8R8A8_SRGB;
            for (uint32_t& p : out)
            {
                if (bgra) p = (p & 0x0000FF00) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
                p |= 0xFF000000;
            }
        }
        readback.Destroy(vk);
        return ok;
    }
    if (useExternal && extTexture)
    {
        // Read the renderer's image back (kept in SHADER_READ_ONLY layout around the copy).
        auto* t = static_cast<vk::Texture*>(extTexture);
        if (screen >= extLayers) return false;
        width = extWidth;
        height = extHeight;
        size_t bytes = (size_t)width * height * 4;
        VkBufferResource readback;
        if (!readback.Create(vk, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true)) return false;
        bool ok = vk.RunOnce([&](VkCommandBuffer cmd) {
            TransitionImage(cmd, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy c {};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)screen, 1};
            c.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
            vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &c);
            TransitionImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        });
        if (ok)
        {
            vmaInvalidateAllocation(vk.Allocator(), readback.allocation, 0, bytes);
            out.resize((size_t)width * height);
            memcpy(out.data(), readback.mapped, bytes);
            for (uint32_t& p : out) p |= 0xFF000000;
        }
        readback.Destroy(vk);
        return ok;
    }
    if (!haveFrame || screen >= softLayers) return false;
    width = softWidth;
    height = softHeight;
    size_t perScreen = (size_t)width * height;
    out.assign(softPixels.begin() + perScreen * screen, softPixels.begin() + perScreen * (screen + 1));
    for (uint32_t& p : out)
    {
        if (softBgra) p = (p & 0x0000FF00) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
        p |= 0xFF000000;
    }
    return true;
}

}
