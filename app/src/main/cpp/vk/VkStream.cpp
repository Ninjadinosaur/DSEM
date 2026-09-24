#include "VkStream.h"
#include "../LogBuffer.h"

#include <algorithm>
#include <cstring>

namespace ds13r::vk
{

namespace
{
VkImageAspectFlags AspectOf(const Texture& t)
{
    return t.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a)
{
    return (v + a - 1) & ~(a - 1);
}
}

Stream::Stream(VulkanContext& vk) : vk(vk)
{
}

Stream::~Stream()
{
    VkDevice dev = vk.Device();
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    for (FrameSlot& s : slots)
    {
        for (auto& f : s.deferredFrees) f();
        s.deferredFrees.clear();
        s.ring.Destroy(vk);
        if (s.descriptors) vkDestroyDescriptorPool(dev, s.descriptors, nullptr);
        if (s.fence) vkDestroyFence(dev, s.fence, nullptr);
        if (s.pool) vkDestroyCommandPool(dev, s.pool, nullptr);
    }
    for (auto& [key, fb] : framebufferCache) vkDestroyFramebuffer(dev, fb, nullptr);
    for (auto& [key, rp] : passCache) vkDestroyRenderPass(dev, rp, nullptr);
    for (VkSampler s : samplers)
        if (s) vkDestroySampler(dev, s, nullptr);
}

bool Stream::Init()
{
    VkDevice dev = vk.Device();
    for (FrameSlot& s : slots)
    {
        VkCommandPoolCreateInfo cpi {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cpi.queueFamilyIndex = vk.QueueFamily();
        if (vkCreateCommandPool(dev, &cpi, nullptr, &s.pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ai {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &ai, &s.cmd);
        VkFenceCreateInfo fi {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev, &fi, nullptr, &s.fence);
        if (!s.ring.Create(vk, kRingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true))
            return false;
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024},
        };
        VkDescriptorPoolCreateInfo dpi {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 2048;
        dpi.poolSizeCount = sizeof(sizes) / sizeof(sizes[0]);
        dpi.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &s.descriptors) != VK_SUCCESS) return false;
    }

    auto makeSampler = [&](VkFilter filter, VkSamplerAddressMode mode) {
        VkSamplerCreateInfo si {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = filter;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = mode;
        si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        si.maxLod = 0.0f;
        VkSampler s = VK_NULL_HANDLE;
        vkCreateSampler(vk.Device(), &si, nullptr, &s);
        return s;
    };
    samplers[(int)SamplerMode::NearestRepeat] = makeSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    samplers[(int)SamplerMode::NearestClampEdge] = makeSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    samplers[(int)SamplerMode::NearestClampBorder] = makeSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    samplers[(int)SamplerMode::LinearClampEdge] = makeSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    return true;
}

// ---------------------------------------------------------------------------
// Frame slots

void Stream::BeginSlot()
{
    FrameSlot& s = Slot();
    if (s.recording) return;
    VkDevice dev = vk.Device();
    if (s.submitted)
    {
        vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &s.fence);
        s.submitted = false;
    }
    for (auto& f : s.deferredFrees) f();
    s.deferredFrees.clear();
    vkResetCommandPool(dev, s.pool, 0);
    vkResetDescriptorPool(dev, s.descriptors, 0);
    s.ringUsed = 0;
    VkCommandBufferBeginInfo bi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    s.recording = true;
}

VkCommandBuffer Stream::Cmd()
{
    BeginSlot();
    return Slot().cmd;
}

void Stream::Flush(bool wait)
{
    FrameSlot& s = Slot();
    if (!s.recording)
    {
        if (wait) vkQueueWaitIdle(vk.Queue());
        return;
    }
    EndRendering();
    vkEndCommandBuffer(s.cmd);
    if (s.ringUsed) vmaFlushAllocation(vk.Allocator(), s.ring.allocation, 0, s.ringUsed);
    VkSubmitInfo si {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    VkResult r = vkQueueSubmit(vk.Queue(), 1, &si, s.fence);
    if (r != VK_SUCCESS) LOGE("Vulkan: renderer submit failed (%s)", VkResultName(r));
    s.recording = false;
    s.submitted = true;
    if (wait)
    {
        vkWaitForFences(vk.Device(), 1, &s.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(vk.Device(), 1, &s.fence);
        s.submitted = false;
        for (auto& f : s.deferredFrees) f();
        s.deferredFrees.clear();
    }
    current = (current + 1) % kSlots;
}

void Stream::RestartSlot()
{
    std::vector<Attachment> colors = currentColors;
    Attachment depthAtt = currentDepth;
    bool inPass = currentPass != VK_NULL_HANDLE;
    VkRect2D sc = scissor;
    Flush(false);
    BeginSlot();
    if (inPass)
    {
        BeginRendering(colors, depthAtt);
        SetScissor(sc.offset.x, sc.offset.y, (int)sc.extent.width, (int)sc.extent.height);
    }
}

void Stream::EnsureSpace(VkDeviceSize bytes)
{
    BeginSlot();
    if (Slot().ringUsed + bytes > kRingSize) RestartSlot();
}

VkDeviceSize Stream::Allocate(VkDeviceSize size, VkDeviceSize alignment, void** mapped)
{
    BeginSlot();
    VkDeviceSize offset = AlignUp(Slot().ringUsed, alignment);
    if (offset + size > kRingSize)
    {
        // This frame's upload space is used up: submit what we have and continue in a fresh slot.
        RestartSlot();
        offset = 0;
    }
    FrameSlot& s = Slot();
    s.ringUsed = offset + size;
    *mapped = static_cast<uint8_t*>(s.ring.mapped) + offset;
    return offset;
}

VkDescriptorSet Stream::AllocateSet(VkDescriptorSetLayout layout)
{
    BeginSlot();
    VkDescriptorSetAllocateInfo ai {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = Slot().descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vk.Device(), &ai, &set) != VK_SUCCESS)
    {
        // Pool exhausted: move on to a fresh slot (keeps the render pass state).
        RestartSlot();
        ai.descriptorPool = Slot().descriptors;
        vkAllocateDescriptorSets(vk.Device(), &ai, &set);
    }
    return set;
}

// ---------------------------------------------------------------------------
// Textures

bool Stream::CreateTexture(Texture& t, const TextureDesc& d)
{
    DestroyTexture(t);
    VkImageCreateInfo ci {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = d.format;
    ci.extent = {d.width, d.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = d.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (d.renderTarget) ci.usage |= d.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (d.transferSrc) ci.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (d.storage) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(vk.Allocator(), &ci, &ai, &t.image, &t.allocation, nullptr) != VK_SUCCESS)
    {
        LOGE("Vulkan: texture %ux%ux%u (format %d) failed", d.width, d.height, d.layers, d.format);
        t = Texture {};
        return false;
    }
    t.format = d.format;
    t.width = d.width;
    t.height = d.height;
    t.layers = d.layers;
    t.depth = d.depth;
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t.lastStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    t.lastAccess = 0;
    t.lastWasWrite = false;

    VkImageViewCreateInfo vi {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = d.array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = d.format;
    vi.components = d.swizzle;
    vi.subresourceRange = {AspectOf(t), 0, 1, 0, d.array ? d.layers : 1};
    vkCreateImageView(vk.Device(), &vi, nullptr, &t.view);

    if (d.renderTarget)
    {
        // Attachment views must not swizzle.
        t.layerViews.resize(d.layers);
        for (uint32_t i = 0; i < d.layers; i++)
        {
            VkImageViewCreateInfo lv = vi;
            lv.viewType = VK_IMAGE_VIEW_TYPE_2D;
            lv.components = {};
            lv.subresourceRange = {AspectOf(t), 0, 1, i, 1};
            vkCreateImageView(vk.Device(), &lv, nullptr, &t.layerViews[i]);
        }
    }
    return true;
}

void Stream::DestroyTexture(Texture& t)
{
    if (!t.Valid()) return;
    EndRendering();
    // Framebuffers referencing this texture's views are invalid now.
    std::vector<VkImageView> views = t.layerViews;
    views.push_back(t.view);
    std::vector<VkFramebuffer> deadFramebuffers;
    for (auto it = framebufferCache.begin(); it != framebufferCache.end();)
    {
        bool uses = false;
        for (uint64_t k : it->first)
            for (VkImageView v : views)
                if (k == (uint64_t)v) uses = true;
        if (uses)
        {
            deadFramebuffers.push_back(it->second);
            it = framebufferCache.erase(it);
        }
        else
        {
            ++it;
        }
    }
    VkDevice dev = vk.Device();
    VmaAllocator alloc = vk.Allocator();
    VkImage image = t.image;
    VmaAllocation allocation = t.allocation;
    auto release = [dev, alloc, image, allocation, views, deadFramebuffers] {
        for (VkFramebuffer fb : deadFramebuffers) vkDestroyFramebuffer(dev, fb, nullptr);
        for (VkImageView v : views)
            if (v) vkDestroyImageView(dev, v, nullptr);
        vmaDestroyImage(alloc, image, allocation);
    };
    // Free once every slot that might use it has finished.
    if (Slot().recording || Slot().submitted) Slot().deferredFrees.push_back(release);
    else release();
    t = Texture {};
    // Other in-flight slots may still reference it too: wait for them conservatively.
    for (FrameSlot& s : slots)
        if (&s != &Slot() && s.submitted)
        {
            vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
        }
}

void Stream::Use(Texture& t, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, bool write)
{
    bool needBarrier = t.layout != layout || t.lastWasWrite || write;
    if (!needBarrier) return;
    if (currentPass) EndRendering();
    VkImageMemoryBarrier b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = t.layout;
    b.newLayout = layout;
    b.srcAccessMask = t.lastWasWrite ? t.lastAccess : 0;
    b.dstAccessMask = access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {AspectOf(t), 0, 1, 0, t.layers};
    vkCmdPipelineBarrier(Cmd(), t.lastStage, stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    t.layout = layout;
    t.lastStage = stage;
    t.lastAccess = access;
    t.lastWasWrite = write;
}

void Stream::UploadTexture(Texture& t, uint32_t layer, int x, int y, int w, int h, const void* data, size_t bpp)
{
    if (w <= 0 || h <= 0) return;
    size_t bytes = (size_t)w * h * bpp;
    void* dst = nullptr;
    VkDeviceSize offset = Allocate(bytes, 16, &dst);
    memcpy(dst, data, bytes);
    Use(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
    VkBufferImageCopy c {};
    c.bufferOffset = offset;
    c.imageSubresource = {AspectOf(t), 0, layer, 1};
    c.imageOffset = {x, y, 0};
    c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(Cmd(), Slot().ring.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void Stream::PrepareForExternalRead(Texture& t)
{
    Use(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
}

bool Stream::ReadTexture(Texture& t, uint32_t layer, int x, int y, int w, int h, void* out, size_t bpp)
{
    size_t bytes = (size_t)w * h * bpp;
    VkBufferResource readback;
    if (!readback.Create(vk, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true)) return false;
    Use(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
    VkBufferImageCopy c {};
    c.imageSubresource = {AspectOf(t), 0, layer, 1};
    c.imageOffset = {x, y, 0};
    c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyImageToBuffer(Cmd(), t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &c);
    VkMemoryBarrier mb {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    Flush(true);
    vmaInvalidateAllocation(vk.Allocator(), readback.allocation, 0, bytes);
    memcpy(out, readback.mapped, bytes);
    readback.Destroy(vk);
    return true;
}

// ---------------------------------------------------------------------------
// Render passes

VkRenderPass Stream::RenderPassFor(const std::vector<VkFormat>& colors, VkFormat depth)
{
    std::vector<uint32_t> key(colors.begin(), colors.end());
    key.push_back((uint32_t)depth);
    auto it = passCache.find(key);
    if (it != passCache.end()) return it->second;

    std::vector<VkAttachmentDescription> atts;
    std::vector<VkAttachmentReference> refs;
    for (size_t i = 0; i < colors.size(); i++)
    {
        VkAttachmentDescription a {};
        a.format = colors[i];
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts.push_back(a);
        refs.push_back({(uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    }
    VkAttachmentReference depthRef {(uint32_t)colors.size(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    if (depth != VK_FORMAT_UNDEFINED)
    {
        VkAttachmentDescription a {};
        a.format = depth;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts.push_back(a);
    }
    VkSubpassDescription sub {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = (uint32_t)refs.size();
    sub.pColorAttachments = refs.data();
    sub.pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &depthRef : nullptr;
    VkRenderPassCreateInfo rpi {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = (uint32_t)atts.size();
    rpi.pAttachments = atts.data();
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    vkCreateRenderPass(vk.Device(), &rpi, nullptr, &rp);
    passCache[key] = rp;
    return rp;
}

VkFramebuffer Stream::FramebufferFor(VkRenderPass pass, const std::vector<VkImageView>& views, uint32_t w, uint32_t h)
{
    std::vector<uint64_t> key;
    key.push_back((uint64_t)pass);
    for (VkImageView v : views) key.push_back((uint64_t)v);
    key.push_back(((uint64_t)w << 32) | h);
    auto it = framebufferCache.find(key);
    if (it != framebufferCache.end()) return it->second;
    VkFramebufferCreateInfo fi {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass;
    fi.attachmentCount = (uint32_t)views.size();
    fi.pAttachments = views.data();
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    vkCreateFramebuffer(vk.Device(), &fi, nullptr, &fb);
    framebufferCache[key] = fb;
    return fb;
}

void Stream::BeginRendering(const std::vector<Attachment>& colors, Attachment depthAttachment)
{
    // Same targets as the pass already open: keep going.
    if (currentPass && colors.size() == currentColors.size() && depthAttachment.texture == currentDepth.texture &&
        depthAttachment.layer == currentDepth.layer)
    {
        bool same = true;
        for (size_t i = 0; i < colors.size(); i++)
            if (colors[i].texture != currentColors[i].texture || colors[i].layer != currentColors[i].layer) same = false;
        if (same) return;
    }
    EndRendering();

    std::vector<VkFormat> formats;
    std::vector<VkImageView> views;
    uint32_t w = 0, h = 0;
    for (const Attachment& a : colors)
    {
        Use(*a.texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, true);
        formats.push_back(a.texture->format);
        views.push_back(a.texture->layerViews[a.layer]);
        w = a.texture->width;
        h = a.texture->height;
    }
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    if (depthAttachment.texture)
    {
        Use(*depthAttachment.texture, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true);
        depthFormat = depthAttachment.texture->format;
        views.push_back(depthAttachment.texture->layerViews[depthAttachment.layer]);
    }

    VkRenderPass pass = RenderPassFor(formats, depthFormat);
    VkRenderPassBeginInfo bi {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = pass;
    bi.framebuffer = FramebufferFor(pass, views, w, h);
    bi.renderArea.extent = {w, h};
    vkCmdBeginRenderPass(Cmd(), &bi, VK_SUBPASS_CONTENTS_INLINE);
    currentPass = pass;
    currentColors = colors;
    currentDepth = depthAttachment;
    passWidth = w;
    passHeight = h;
    SetViewport(0, 0, (int)w, (int)h);
    SetScissor(0, 0, (int)w, (int)h);
}

void Stream::EndRendering()
{
    if (!currentPass) return;
    vkCmdEndRenderPass(Slot().cmd);
    currentPass = VK_NULL_HANDLE;
    currentColors.clear();
    currentDepth = {};
}

void Stream::SetViewport(int x, int y, int w, int h)
{
    VkViewport vp {(float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f};
    vkCmdSetViewport(Cmd(), 0, 1, &vp);
}

void Stream::SetScissor(int x, int y, int w, int h)
{
    // Clip to the render area (GL allows scissors outside the framebuffer).
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min((int)passWidth, x + w), y1 = std::min((int)passHeight, y + h);
    scissor.offset = {x0, y0};
    scissor.extent = {(uint32_t)std::max(0, x1 - x0), (uint32_t)std::max(0, y1 - y0)};
    vkCmdSetScissor(Cmd(), 0, 1, &scissor);
}

void Stream::ClearColor(uint32_t attachment, float r, float g, float b, float a)
{
    if (!currentPass || scissor.extent.width == 0 || scissor.extent.height == 0) return;
    VkClearAttachment ca {};
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = attachment;
    ca.clearValue.color = {{r, g, b, a}};
    VkClearRect rect {scissor, 0, 1};
    vkCmdClearAttachments(Cmd(), 1, &ca, 1, &rect);
}

void Stream::ClearDepth(float depth)
{
    if (!currentPass || !currentDepth.texture || scissor.extent.width == 0 || scissor.extent.height == 0) return;
    VkClearAttachment ca {};
    ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ca.clearValue.depthStencil = {depth, 0};
    VkClearRect rect {scissor, 0, 1};
    vkCmdClearAttachments(Cmd(), 1, &ca, 1, &rect);
}

void Stream::Draw(VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSetLayout setLayout,
                  const std::vector<Binding>& bindings, const void* pushConstants, uint32_t pushSize,
                  const void* vertices, size_t vertexBytes, uint32_t vertexCount)
{
    if (!currentPass || vertexCount == 0) return;

    // Sampled textures must be readable; if any needs a barrier, it goes outside the pass.
    bool needsBarrier = false;
    for (const Binding& b : bindings)
    {
        if (!b.texture) continue;
        Texture& t = *b.texture;
        if (t.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || t.lastWasWrite) needsBarrier = true;
    }
    if (needsBarrier)
    {
        std::vector<Attachment> colors = currentColors;
        Attachment depthAtt = currentDepth;
        VkRect2D sc = scissor;
        EndRendering();
        for (const Binding& b : bindings)
            if (b.texture)
                Use(*b.texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    false);
        BeginRendering(colors, depthAtt);
        SetScissor(sc.offset.x, sc.offset.y, (int)sc.extent.width, (int)sc.extent.height);
    }

    // Reserve this draw's upload space first, so nothing below can trigger a slot change.
    VkDeviceSize uboAlign = std::max<VkDeviceSize>(16, vk.Properties().limits.minUniformBufferOffsetAlignment);
    VkDeviceSize needed = AlignUp(vertexBytes, 16) + 16;
    for (const Binding& b : bindings)
        if (!b.texture) needed += AlignUp(b.uniformSize, uboAlign) + uboAlign;
    EnsureSpace(needed);

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (setLayout)
    {
        set = AllocateSet(setLayout);
        std::vector<VkDescriptorImageInfo> images;
        std::vector<VkDescriptorBufferInfo> buffers;
        images.reserve(bindings.size());
        buffers.reserve(bindings.size());
        std::vector<VkWriteDescriptorSet> writes;
        for (const Binding& b : bindings)
        {
            VkWriteDescriptorSet w {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = b.binding;
            w.dstArrayElement = b.arrayIndex;
            w.descriptorCount = 1;
            if (b.texture)
            {
                images.push_back({Sampler(b.sampler), b.texture->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &images.back();
            }
            else
            {
                void* dst = nullptr;
                VkDeviceSize offset = Allocate(b.uniformSize, uboAlign, &dst);
                memcpy(dst, b.uniformData, b.uniformSize);
                buffers.push_back({Slot().ring.buffer, offset, b.uniformSize});
                w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w.pBufferInfo = &buffers.back();
            }
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(vk.Device(), (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }

    VkCommandBuffer cmd = Cmd();
    if (vertices && vertexBytes)
    {
        void* dst = nullptr;
        VkDeviceSize offset = Allocate(vertexBytes, 16, &dst);
        memcpy(dst, vertices, vertexBytes);
        vkCmdBindVertexBuffers(cmd, 0, 1, &Slot().ring.buffer, &offset);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (set) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    if (pushSize)
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, pushConstants);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, vertexCount, 1, 0, 0);
}

// ---------------------------------------------------------------------------
// Buffers and compute

void Stream::MemoryBarrier(VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                           VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    EndRendering();
    VkMemoryBarrier mb {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = srcAccess;
    mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(Cmd(), srcStage, dstStage, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void Stream::UploadBuffer(VkBuffer dst, VkDeviceSize offset, const void* data, VkDeviceSize size)
{
    if (size == 0) return;
    void* mapped = nullptr;
    VkDeviceSize src = Allocate(size, 16, &mapped);
    memcpy(mapped, data, size);
    // Earlier shader reads/writes of the buffer must finish before it is overwritten.
    MemoryBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferCopy c {src, offset, size};
    vkCmdCopyBuffer(Cmd(), Slot().ring.buffer, dst, 1, &c);
    MemoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT);
}

VkDescriptorSet Stream::AllocateDescriptorSet(VkDescriptorSetLayout layout)
{
    EndRendering();
    return AllocateSet(layout);
}

// ---------------------------------------------------------------------------
// Pipelines

VkPipeline CreateGraphicsPipeline(VulkanContext& vk, const PipelineDesc& d)
{
    VkShaderModule vs = vk.CreateShaderModule(d.vertCode, d.vertBytes);
    VkShaderModule fs = vk.CreateShaderModule(d.fragCode, d.fragBytes);
    VkPipelineShaderStageCreateInfo stages[2] {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vb {0, d.vertexStride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (d.vertexStride)
    {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = (uint32_t)d.attribs.size();
        vi.pVertexAttributeDescriptions = d.attribs.data();
    }
    VkPipelineInputAssemblyStateCreateInfo ia {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkPipelineColorBlendAttachmentState> blends(d.colorMasks.size());
    for (size_t i = 0; i < d.colorMasks.size(); i++) blends[i].colorWriteMask = d.colorMasks[i];
    VkPipelineColorBlendStateCreateInfo cb {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = (uint32_t)blends.size();
    cb.pAttachments = blends.data();
    VkPipelineDepthStencilStateCreateInfo ds {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depthTest;
    ds.depthWriteEnable = d.depthWrite;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pi {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = d.layout;
    pi.renderPass = d.renderPass;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(vk.Device(), vk.PipelineCache(), 1, &pi, nullptr, &p);
    if (r != VK_SUCCESS) LOGE("Vulkan: pipeline creation failed (%s)", VkResultName(r));
    vkDestroyShaderModule(vk.Device(), vs, nullptr);
    vkDestroyShaderModule(vk.Device(), fs, nullptr);
    return p;
}

VkDescriptorSetLayout CreateSetLayout(VulkanContext& vk, const std::vector<LayoutBinding>& bindings)
{
    std::vector<VkDescriptorSetLayoutBinding> bs;
    for (const LayoutBinding& b : bindings)
    {
        VkDescriptorSetLayoutBinding x {};
        x.binding = b.binding;
        x.descriptorType = b.type;
        x.descriptorCount = b.count;
        x.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        bs.push_back(x);
    }
    VkDescriptorSetLayoutCreateInfo ci {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = (uint32_t)bs.size();
    ci.pBindings = bs.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(vk.Device(), &ci, nullptr, &l);
    return l;
}

VkPipelineLayout CreatePipelineLayout(VulkanContext& vk, VkDescriptorSetLayout set, uint32_t pushSize)
{
    VkPushConstantRange pcr {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize};
    VkPipelineLayoutCreateInfo ci {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = set ? 1 : 0;
    ci.pSetLayouts = &set;
    ci.pushConstantRangeCount = pushSize ? 1 : 0;
    ci.pPushConstantRanges = &pcr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    vkCreatePipelineLayout(vk.Device(), &ci, nullptr, &l);
    return l;
}

}
