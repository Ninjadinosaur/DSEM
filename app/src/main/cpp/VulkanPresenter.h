#pragma once

#include "Presenter.h"
#include "VulkanContext.h"

#include <jni.h>
#include <mutex>
#include <vector>

namespace ds13r
{

// Vulkan presenter: draws the console screens onto the phone's surface with the same filters
// and layout as GLPresenter. Follows Android's Vulkan guidance: the swapchain is pre-rotated
// (we draw in the panel's native orientation so the compositor never rotates our frames), and
// the Android Frame Pacing library (SwappyVk) times presentation against vsync.
class VulkanPresenter : public Presenter
{
public:
    VulkanPresenter(VulkanContext& vk, JavaVM* vm, jobject activity);
    ~VulkanPresenter() override;

    bool Init() override;
    void Shutdown() override;

    void SetWindow(ANativeWindow* window) override;
    void ReleaseWindow() override;
    bool HasWindow() const override { return swapchain != VK_NULL_HANDLE; }

    void SetLayout(const PresentLayout& layout) override;
    void SetSettings(const PresentSettings& settings) override;

    void UploadSoftwareFrame(const void* const* screens, int count, int width, int height, bool bgra) override;
    // A frame rendered by the DS Vulkan renderer (a ds13r::vk::Texture*, one layer per screen,
    // already in SHADER_READ_ONLY layout). Sampled directly; no copy.
    void SetExternalFrame(void* texture, int width, int height, int layers);
    // A frame from another Vulkan renderer on this device (the 3DS engine): a 2D image in
    // SHADER_READ_ONLY layout, sampled through a view we create per frame.
    void SetExternalImage(VkImage image, VkFormat format, int width, int height);
    // Forgets the external image (its owner destroyed or replaced it) until the next SetExternalImage.
    void DropExternalImage();

    // Frame slots, for renderers that must know when we're done reading their images
    // (libretro's Vulkan sync index). The next Present() uses CurrentSlot().
    int CurrentSlot() const { return frameIndex; }
    int SlotCount() const { return kFramesInFlight; }
    void WaitSlot(int slot);

    bool Present() override;
    void SetTargetRefreshRate(float hz) override;

    bool ReadScreen(int screen, std::vector<uint32_t>& out, int& width, int& height) override;

private:
    static constexpr int kFramesInFlight = 2;

    struct Frame
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkBufferResource staging;      // software frame upload
        VkImageResource texture;       // the screens, one array layer each
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t textureVersion = 0;   // which software frame the texture holds
        bool textureReady = false;     // has been written at least once (layout is SHADER_READ)
        VkImageView boundView = VK_NULL_HANDLE; // what the descriptor set currently points at
        VkImageView externalView = VK_NULL_HANDLE; // per-frame view of an external image
    };

    bool CreatePipeline();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool PrepareFrameTexture(Frame& f);
    void BindView(Frame& f, VkImageView view);

    VulkanContext& vk;
    JavaVM* vm;
    jobject activity;
    bool initialized = false;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkFormat surfaceFormat = VK_FORMAT_UNDEFINED;

    Frame frames[kFramesInFlight];
    int frameIndex = 0;

    // Window and swapchain
    ANativeWindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent {};                   // native (identity) orientation
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    int logicalWidth = 0, logicalHeight = 0; // as the app sees the surface
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderDone;    // one per swapchain image
    bool swappyReady = false;
    bool needRecreate = false;

    // Latest software frame (kept on the CPU so either frame slot can be refreshed from it)
    std::vector<uint32_t> softPixels;
    int softWidth = 0, softHeight = 0, softLayers = 0;
    bool softBgra = true;
    uint64_t softVersion = 0;
    bool haveFrame = false;

    // External (Vulkan-rendered) frame
    void* extTexture = nullptr;
    int extWidth = 0, extHeight = 0, extLayers = 0;
    bool useExternal = false;
    VkImage extImage = VK_NULL_HANDLE;
    VkFormat extFormat = VK_FORMAT_UNDEFINED;
    bool useExternalImage = false;

    std::mutex layoutLock;
    PresentLayout layout;
    PresentSettings settings;

    float targetHz = 60.0f;
};

}
