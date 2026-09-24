#pragma once

#include "../VulkanContext.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

// A thin OpenGL-style command layer over Vulkan for the DS renderer port. melonDS's GL renderer
// issues many small operations through the frame (texture uploads, render-target switches,
// uniform updates between draws, synchronous read-backs); this layer lets the Vulkan port keep
// that structure while handling what GL did implicitly:
//   - image layouts and hazards (barriers are inserted on use),
//   - upload memory (a per-frame ring buffer; nothing is overwritten while the GPU reads it),
//   - descriptor sets (allocated per draw from a per-frame pool),
//   - render passes and framebuffers (created on demand and cached),
//   - Flush(): submit, optionally wait (replaces glReadPixels' implicit sync).
namespace ds13r::vk
{

// An image the stream tracks: current layout and last write, for automatic barriers.
struct Texture
{
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;          // all layers (2D_ARRAY) or the one layer (2D)
    std::vector<VkImageView> layerViews;         // one 2D view per layer, for render targets
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, layers = 0;
    bool depth = false;

    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags lastStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags lastAccess = 0;
    bool lastWasWrite = false;

    bool Valid() const { return image != VK_NULL_HANDLE; }
};

struct TextureDesc
{
    uint32_t width = 0, height = 0, layers = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    bool array = false;        // sampled as sampler2DArray (else sampler2D; layers must be 1)
    bool renderTarget = false;
    bool depth = false;
    bool transferSrc = false;  // read back to the CPU
    VkComponentMapping swizzle {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
};

enum class SamplerMode
{
    NearestRepeat,
    NearestClampEdge,
    NearestClampBorder, // transparent black border, like GL's default border colour
    LinearClampEdge,
};

// One colour or depth attachment: a texture layer.
struct Attachment
{
    Texture* texture = nullptr;
    uint32_t layer = 0;
};

// Binding values for one draw's descriptor set.
struct Binding
{
    uint32_t binding = 0;
    uint32_t arrayIndex = 0;
    Texture* texture = nullptr;       // image binding (with `sampler`)
    SamplerMode sampler = SamplerMode::NearestClampEdge;
    const void* uniformData = nullptr; // uniform buffer binding: uploaded per draw
    size_t uniformSize = 0;
};

class Stream
{
public:
    explicit Stream(VulkanContext& vk);
    ~Stream();

    bool Init();
    VulkanContext& Context() { return vk; }

    // ---- resources
    bool CreateTexture(Texture& t, const TextureDesc& desc);
    void DestroyTexture(Texture& t);  // deferred until the GPU is done with it
    VkSampler Sampler(SamplerMode mode) const { return samplers[(int)mode]; }

    // ---- uploads (recorded into the current command buffer)
    void UploadTexture(Texture& t, uint32_t layer, int x, int y, int w, int h, const void* data, size_t bytesPerPixel);

    // ---- rendering
    // Starts (or keeps) a render pass on these attachments. Contents are preserved (GL-like).
    void BeginRendering(const std::vector<Attachment>& colors, Attachment depthAttachment = {});
    void EndRendering();
    void SetViewport(int x, int y, int w, int h);
    void SetScissor(int x, int y, int w, int h);
    // Clears the current attachments inside the scissor rect (like glClear with scissor test).
    void ClearColor(uint32_t attachment, float r, float g, float b, float a);
    void ClearDepth(float depth);

    // Draw with a pipeline (see CreateGraphicsPipeline) and per-draw bindings.
    void Draw(VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSetLayout setLayout,
              const std::vector<Binding>& bindings, const void* pushConstants, uint32_t pushSize,
              const void* vertices, size_t vertexBytes, uint32_t vertexCount);

    // Render pass compatible with the current attachments (for pipeline creation).
    VkRenderPass CurrentRenderPass() const { return currentPass; }
    VkRenderPass RenderPassFor(const std::vector<VkFormat>& colors, VkFormat depth);

    // ---- read-back
    // Copies a texture region into host memory after all prior work; waits for the GPU.
    bool ReadTexture(Texture& t, uint32_t layer, int x, int y, int w, int h, void* out, size_t bytesPerPixel);

    // ---- synchronisation
    // Makes a texture ready to be sampled by a later submission (e.g. the presenter).
    void PrepareForExternalRead(Texture& t);
    // Submits recorded work. wait=true blocks until the GPU finished it.
    void Flush(bool wait);

    // Tracks and inserts the barrier needed before the next use of `t`.
    void Use(Texture& t, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, bool write);

    VkCommandBuffer Cmd();

private:
    struct FrameSlot
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkBufferResource ring;
        VkDeviceSize ringUsed = 0;
        VkDescriptorPool descriptors = VK_NULL_HANDLE;
        std::vector<std::function<void()>> deferredFrees;
        bool submitted = false;
        bool recording = false;
    };
    static constexpr int kSlots = 3;
    static constexpr VkDeviceSize kRingSize = 16 * 1024 * 1024;

    FrameSlot& Slot() { return slots[current]; }
    void BeginSlot();
    // Submits this slot and continues in the next one, re-opening the current render pass.
    void RestartSlot();
    // Makes sure `bytes` (plus alignment slack) fit in this slot's ring buffer.
    void EnsureSpace(VkDeviceSize bytes);
    // Space in this frame's ring buffer; flushes if full.
    VkDeviceSize Allocate(VkDeviceSize size, VkDeviceSize alignment, void** mapped);
    VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout);
    VkFramebuffer FramebufferFor(VkRenderPass pass, const std::vector<VkImageView>& views, uint32_t w, uint32_t h);

    VulkanContext& vk;
    FrameSlot slots[kSlots];
    int current = 0;
    VkSampler samplers[4] {};

    // Render pass state
    VkRenderPass currentPass = VK_NULL_HANDLE;
    std::vector<Attachment> currentColors;
    Attachment currentDepth;
    uint32_t passWidth = 0, passHeight = 0;
    VkRect2D scissor {};

    std::map<std::vector<uint32_t>, VkRenderPass> passCache;
    std::map<std::vector<uint64_t>, VkFramebuffer> framebufferCache;
};

// Creates a pipeline for melonDS's shaders: triangle list, no culling, optional blending off
// (melonDS's 2D path never blends in hardware), per-attachment colour write masks, optional
// depth test. `attribs` describes integer/float vertex attributes of one interleaved buffer.
struct PipelineDesc
{
    const uint32_t* vertCode = nullptr;
    size_t vertBytes = 0;
    const uint32_t* fragCode = nullptr;
    size_t fragBytes = 0;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    uint32_t vertexStride = 0;
    std::vector<VkVertexInputAttributeDescription> attribs;
    std::vector<VkColorComponentFlags> colorMasks; // one per colour attachment
    bool depthTest = false;
    bool depthWrite = false;
};
VkPipeline CreateGraphicsPipeline(VulkanContext& vk, const PipelineDesc& desc);

// Descriptor set layout and pipeline layout from a simple binding list.
struct LayoutBinding
{
    uint32_t binding;
    VkDescriptorType type;
    uint32_t count = 1;
};
VkDescriptorSetLayout CreateSetLayout(VulkanContext& vk, const std::vector<LayoutBinding>& bindings);
VkPipelineLayout CreatePipelineLayout(VulkanContext& vk, VkDescriptorSetLayout set, uint32_t pushSize);

}
