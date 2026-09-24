#define VMA_IMPLEMENTATION
#include "VulkanContext.h"

#include "IoWorker.h"
#include "LogBuffer.h"

#include <swappy/swappyVk.h>

#include <cstring>

namespace ds13r
{

const char* VkResultName(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VK_ERROR_(other)";
    }
}

VulkanContext::~VulkanContext()
{
    Shutdown();
}

bool VulkanContext::Init(const std::string& cacheDir)
{
    if (Ready()) return true;

    VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DS13R";
    app.apiVersion = VK_API_VERSION_1_1;
    const char* instanceExts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instanceExts;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS)
    {
        LOGE("Vulkan: vkCreateInstance failed (%s)", VkResultName(r));
        return false;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0)
    {
        LOGE("Vulkan: no GPU");
        Shutdown();
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    physicalDevice = gpus[0];
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    // One queue that does graphics, compute and presentation (true on every Android GPU).
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, families.data());
    bool found = false;
    for (uint32_t i = 0; i < count; i++)
    {
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
        {
            queueFamily = i;
            found = true;
            break;
        }
    }
    if (!found)
    {
        LOGE("Vulkan: no graphics+compute queue");
        Shutdown();
        return false;
    }

    // Device extensions: the swapchain, plus whatever the frame pacing library wants.
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> available(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, available.data());
    uint32_t swappyCount = 0;
    SwappyVk_determineDeviceExtensions(physicalDevice, extCount, available.data(), &swappyCount, nullptr);
    std::vector<std::vector<char>> swappyNameStore(swappyCount, std::vector<char>(VK_MAX_EXTENSION_NAME_SIZE));
    std::vector<char*> swappyNames(swappyCount);
    for (uint32_t i = 0; i < swappyCount; i++) swappyNames[i] = swappyNameStore[i].data();
    SwappyVk_determineDeviceExtensions(physicalDevice, extCount, available.data(), &swappyCount, swappyNames.data());

    std::vector<const char*> deviceExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    for (uint32_t i = 0; i < swappyCount; i++)
        if (strcmp(swappyNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) != 0) deviceExts.push_back(swappyNames[i]);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)deviceExts.size();
    dci.ppEnabledExtensionNames = deviceExts.data();
    r = vkCreateDevice(physicalDevice, &dci, nullptr, &device);
    if (r != VK_SUCCESS)
    {
        LOGE("Vulkan: vkCreateDevice failed (%s)", VkResultName(r));
        Shutdown();
        return false;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    SwappyVk_setQueueFamilyIndex(device, queue, queueFamily);

    VmaVulkanFunctions fns {};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci {};
    aci.vulkanApiVersion = VK_API_VERSION_1_1;
    aci.physicalDevice = physicalDevice;
    aci.device = device;
    aci.instance = instance;
    aci.pVulkanFunctions = &fns;
    r = vmaCreateAllocator(&aci, &allocator);
    if (r != VK_SUCCESS)
    {
        LOGE("Vulkan: vmaCreateAllocator failed (%s)", VkResultName(r));
        Shutdown();
        return false;
    }

    // Pipeline cache from the last run, if it was made by this exact GPU and driver.
    cachePath = cacheDir + "/vk_pipeline_cache.bin";
    std::vector<unsigned char> blob;
    bool usable = false;
    if (ReadWholeFile(cachePath, blob) && blob.size() >= 16 + VK_UUID_SIZE)
    {
        uint32_t vendor, deviceId;
        memcpy(&vendor, blob.data() + 8, 4);
        memcpy(&deviceId, blob.data() + 12, 4);
        usable = vendor == properties.vendorID && deviceId == properties.deviceID &&
                 memcmp(blob.data() + 16, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    }
    VkPipelineCacheCreateInfo pci {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (usable)
    {
        pci.initialDataSize = blob.size();
        pci.pInitialData = blob.data();
    }
    if (vkCreatePipelineCache(device, &pci, nullptr, &pipelineCache) != VK_SUCCESS)
    {
        pci.initialDataSize = 0;
        pci.pInitialData = nullptr;
        vkCreatePipelineCache(device, &pci, nullptr, &pipelineCache);
    }

    VkCommandPoolCreateInfo cpi {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpi.queueFamilyIndex = queueFamily;
    vkCreateCommandPool(device, &cpi, nullptr, &oncePool);

    LOGI("Vulkan: %s, API %u.%u.%u, driver 0x%x, pipeline cache %s", properties.deviceName,
         VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
         VK_VERSION_PATCH(properties.apiVersion), properties.driverVersion, usable ? "loaded" : "new");
    return true;
}

void VulkanContext::SavePipelineCache()
{
    if (!pipelineCache) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device, pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0) return;
    std::vector<unsigned char> data(size);
    if (vkGetPipelineCacheData(device, pipelineCache, &size, data.data()) != VK_SUCCESS) return;
    data.resize(size);
    WriteFileAtomic(cachePath, data.data(), data.size());
}

void VulkanContext::Shutdown()
{
    if (device)
    {
        vkDeviceWaitIdle(device);
        SavePipelineCache();
        if (oncePool) vkDestroyCommandPool(device, oncePool, nullptr);
        if (pipelineCache) vkDestroyPipelineCache(device, pipelineCache, nullptr);
        if (allocator) vmaDestroyAllocator(allocator);
        SwappyVk_destroyDevice(device);
        vkDestroyDevice(device, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
    oncePool = VK_NULL_HANDLE;
    pipelineCache = VK_NULL_HANDLE;
    allocator = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    physicalDevice = VK_NULL_HANDLE;
}

VkShaderModule VulkanContext::CreateShaderModule(const uint32_t* code, size_t bytes) const
{
    VkShaderModuleCreateInfo ci {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) LOGE("Vulkan: shader module failed");
    return m;
}

VkCommandBuffer VulkanContext::BeginOnce()
{
    VkCommandBufferAllocateInfo ai {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = oncePool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

bool VulkanContext::EndOnce(VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) r = vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, oncePool, 1, &cmd);
    if (r != VK_SUCCESS) LOGE("Vulkan: one-shot submit failed (%s)", VkResultName(r));
    return r == VK_SUCCESS;
}

bool VkImageResource::Create(VulkanContext& vk, uint32_t w, uint32_t h, uint32_t n, VkFormat fmt, VkImageUsageFlags usage)
{
    Destroy(vk);
    VkImageCreateInfo ci {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = n;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(vk.Allocator(), &ci, &ai, &image, &allocation, nullptr) != VK_SUCCESS)
    {
        LOGE("Vulkan: image %ux%ux%u failed", w, h, n);
        image = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo vi {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, n};
    vkCreateImageView(vk.Device(), &vi, nullptr, &view);
    width = w;
    height = h;
    layers = n;
    format = fmt;
    return true;
}

void VkImageResource::Destroy(VulkanContext& vk)
{
    if (view) vkDestroyImageView(vk.Device(), view, nullptr);
    if (image) vmaDestroyImage(vk.Allocator(), image, allocation);
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    width = height = layers = 0;
}

bool VkBufferResource::Create(VulkanContext& vk, VkDeviceSize bytes, VkBufferUsageFlags usage, bool hostVisible, bool readback)
{
    Destroy(vk);
    VkBufferCreateInfo ci {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = bytes;
    ci.usage = usage;
    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible)
    {
        ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    }
    VmaAllocationInfo info {};
    if (vmaCreateBuffer(vk.Allocator(), &ci, &ai, &buffer, &allocation, &info) != VK_SUCCESS)
    {
        LOGE("Vulkan: buffer of %llu bytes failed", (unsigned long long)bytes);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    mapped = info.pMappedData;
    size = bytes;
    return true;
}

void VkBufferResource::Destroy(VulkanContext& vk)
{
    if (buffer) vmaDestroyBuffer(vk.Allocator(), buffer, allocation);
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    mapped = nullptr;
    size = 0;
}

void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                     VkPipelineStageFlags dstStage, VkAccessFlags dstAccess, uint32_t layers)
{
    VkImageMemoryBarrier b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}
