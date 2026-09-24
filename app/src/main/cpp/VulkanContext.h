#pragma once

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Vulkan Memory Allocator: sub-allocates device memory for our images and buffers.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

namespace ds13r
{

// The Vulkan instance, device and queue, shared by the Vulkan presenter and the DS Vulkan
// renderer. Created on the emulation thread and used only there (one graphics+compute queue).
class VulkanContext
{
public:
    ~VulkanContext();

    // `cacheDir` is where the pipeline cache is kept between runs.
    bool Init(const std::string& cacheDir);
    void Shutdown();
    bool Ready() const { return device != VK_NULL_HANDLE; }

    VkInstance Instance() const { return instance; }
    VkPhysicalDevice PhysicalDevice() const { return physicalDevice; }
    VkDevice Device() const { return device; }
    VkQueue Queue() const { return queue; }
    uint32_t QueueFamily() const { return queueFamily; }
    VmaAllocator Allocator() const { return allocator; }
    VkPipelineCache PipelineCache() const { return pipelineCache; }
    const VkPhysicalDeviceProperties& Properties() const { return properties; }

    // Writes the pipeline cache to disk (after new pipelines were created).
    void SavePipelineCache();

    // The queue is shared with the 3DS engine, which may submit from its own thread: every
    // submit/present/wait-idle on it must hold this lock (Vulkan requires external sync).
    void LockQueue() { queueLock.lock(); }
    void UnlockQueue() { queueLock.unlock(); }
    VkResult Submit(const VkSubmitInfo& info, VkFence fence);
    VkResult WaitIdle();
    void DeviceWaitIdle();

    VkShaderModule CreateShaderModule(const uint32_t* code, size_t bytes) const;

    // Records and submits a short command buffer, and waits for it to finish.
    template <typename F> bool RunOnce(F&& record)
    {
        VkCommandBuffer cmd = BeginOnce();
        if (!cmd) return false;
        record(cmd);
        return EndOnce(cmd);
    }

private:
    VkCommandBuffer BeginOnce();
    bool EndOnce(VkCommandBuffer cmd);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkCommandPool oncePool = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties {};
    std::string cachePath;
    std::recursive_mutex queueLock;
};

// An image plus its memory and a view.
struct VkImageResource
{
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, layers = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;

    bool Create(VulkanContext& vk, uint32_t w, uint32_t h, uint32_t layers, VkFormat fmt, VkImageUsageFlags usage);
    void Destroy(VulkanContext& vk);
};

// A buffer plus its memory; host-visible buffers stay mapped.
struct VkBufferResource
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;

    bool Create(VulkanContext& vk, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, bool readback = false);
    void Destroy(VulkanContext& vk);
};

// Layout transition helper (synchronization2-free, works on Vulkan 1.1).
void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                     VkPipelineStageFlags dstStage, VkAccessFlags dstAccess,
                     uint32_t layers = VK_REMAINING_ARRAY_LAYERS);

const char* VkResultName(VkResult r);

}
