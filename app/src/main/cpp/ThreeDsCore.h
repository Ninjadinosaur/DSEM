#pragma once

#include "EmuCore.h"

#include <GLES3/gl32.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ds13r
{

struct LibretroApi;

// Nintendo 3DS, emulated by Azahar through its libretro core (libazahar_libretro.so).
// The core is loaded only when a 3DS game starts. It renders with OpenGL ES into a framebuffer
// we own (in the presenter's context), composing both 3DS screens into one image; we copy that
// into the presenter each frame. Audio arrives at the 3DS's 32,728 Hz and is resampled to 48 kHz.
class ThreeDsCore : public EmuCore
{
public:
    explicit ThreeDsCore(CoreHost& host);
    ~ThreeDsCore() override;

    // Takes its own duplicate of `fd` (kept open while the game runs: the engine streams the
    // file rather than reading it whole). `extension` is e.g. ".3ds". Returns "" or a message.
    std::string LoadGame(int fd, const std::string& extension, const std::string& gameKey);

    const char* SystemName() const override { return "3DS"; }
    double NativeFps() const override { return 59.8261; }
    void ApplySettings() override;
    void RunFrame(const CoreInput& input) override;
    bool GetFrame(FrameInfo& out) override;
    void Reset() override;
    void Shutdown() override;
    void CheckFlush() override {}
    void FlushSaves() override {}
    bool SaveState(std::vector<uint8_t>& out) override;
    bool LoadState(const uint8_t* data, size_t len) override;
    // A 3DS snapshot includes 128+ MB of RAM: too big for continuous rewind.
    size_t CaptureState(uint8_t*, size_t) override { return 0; }
    size_t MaxStateSize() const override { return 0; }
    void SetCheats(const std::vector<std::string>& codes) override;
    void SetAudioSkew(double skew) override { audioSkew = skew; }
    int AudioFill() override;
    int ReadAudio(int16_t* out, int frames) override;
    int RenderScale() const override;
    bool UsesHardwareRenderer() const override { return true; }
    bool WantsVulkanOutput() const override { return useVulkan; }

    // Size of the composed frame (both screens) the core produces, for the app's layout.
    void FrameSize(int& width, int& height) const { width = frameW; height = frameH; }

    // ---- libretro callbacks (static trampolines forward here) ----
    bool Environment(unsigned cmd, void* data);
    void VideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch);
    size_t AudioBatch(const int16_t* data, size_t frames);
    int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id);
    uintptr_t CurrentFramebuffer() const { return fbo; }

    CoreHost& Host() { return host; }

    // ---- libretro Vulkan interface (retro_hw_render_interface_vulkan callbacks)
    struct VulkanBridge;
    VulkanBridge& Bridge() { return *vkBridge; }
    // The game file, which the engine's file access serves from our descriptor.
    bool IsRomPath(const char* path) const { return !romLink.empty() && romLink == path; }
    int RomFd() const { return romFd; }

private:
    bool LoadLibrary();
    void CreateFramebuffer(int width, int height);
    void DestroyFramebuffer();
    void CopyToPresenterTexture(unsigned width, unsigned height);
    void ForgetVulkanImage();
    std::string OptionOverride(const std::string& key) const;

    CoreHost& host;
    std::unique_ptr<LibretroApi> api;
    bool gameLoaded = false;
    std::string systemDir, saveDir;
    std::string lastMessage;
    int romFd = -1;
    std::string romLink;

    // Core options: defaults declared by the core, values possibly overridden by us.
    std::map<std::string, std::string> options;
    bool optionsChanged = false;

    // Rendering
    struct HwRender;
    std::unique_ptr<HwRender> hw;
    bool useVulkan = false;                  // Azahar renders with Vulkan (else OpenGL ES)
    std::unique_ptr<VulkanBridge> vkBridge;
    GLuint fbo = 0, fboColor = 0, fboDepth = 0;
    int fboW = 0, fboH = 0;
    GLuint outTexture = 0;   // 1-layer texture array handed to the presenter
    int outW = 0, outH = 0;
    int frameW = 400, frameH = 480;
    bool haveFrame = false;
    bool softwareFrame = false;
    std::vector<uint32_t> softPixels;

    // Input for the current frame
    CoreInput input;

    // Audio: resampled to 48 kHz into a ring the audio thread drains.
    std::mutex audioLock;
    std::vector<int16_t> ring;
    size_t ringRead = 0, ringWrite = 0, ringLevel = 0;
    double resamplePos = 0.0;
    int16_t prevL = 0, prevR = 0;
    double audioSkew = 1.0;
    double coreSampleRate = 32728.0;
};

}
