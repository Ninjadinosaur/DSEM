#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One emulated system (Nintendo DS via melonDS, Game Boy Advance via mGBA).
// EmuSession owns the shared machinery - the emulation thread, frame pacing, display, audio
// output, input, save states, rewind, fast-forward and thermal policy - and drives a core
// through this interface, so every feature works the same for both systems.
namespace ds13r
{

class ConfigStore;
class GLPresenter;
class VulkanContext;
class VulkanPresenter;

// Pulled by the audio thread; stereo 16-bit at 48 kHz.
class AudioSource
{
public:
    virtual ~AudioSource() = default;
    virtual int ReadAudio(int16_t* out, int frames) = 0;
};

// Services a core needs from the app.
class CoreHost
{
public:
    virtual ~CoreHost() = default;
    virtual ConfigStore& Config() = 0;
    virtual const std::string& FilesDir() const = 0;
    virtual GLPresenter* Presenter() = 0;
    // The shared Vulkan device (created on first use); null if Vulkan is unavailable.
    virtual VulkanContext* Vulkan() = 0;
    // The Vulkan presenter (created on first use, even before it owns the window).
    virtual VulkanPresenter* VulkanOutput() = 0;
    virtual void OnMicStart() = 0;
    virtual void OnMicStop() = 0;
    virtual int ReadMic(int16_t* data, int maxlen) = 0;
    virtual void OnRumble(uint32_t ms) = 0;
    virtual void OnRtcOffset(int64_t offsetSeconds) = 0;
    virtual void OnCoreStopped(int reason) = 0;
    // Phone sensors standing in for cartridge accessories (features.md §10).
    virtual float MotionQuery(int type) = 0;   // 0-2 acceleration m/s^2, 3-5 rotation rad/s
    virtual bool GuitarKeyDown(int key) = 0;
    virtual float LightLevel() = 0;            // ambient light 0..1 (Boktai solar sensor)
};

// Input for one frame. Keys use the DS layout, active-high:
// bit 0 A, 1 B, 2 Select, 3 Start, 4 Right, 5 Left, 6 Up, 7 Down, 8 R, 9 L, 10 X, 11 Y.
// (Bits 0-9 are also exactly the GBA's key order.)
struct CoreInput
{
    // 3DS extras above the DS keys: bit 12 ZL, 13 ZR, 14 HOME.
    uint32_t keys = 0;
    bool touching = false;
    int touchX = 0, touchY = 0;
    int lidRequest = -1; // -1 = no change, 0 = open, 1 = close
    // 3DS: circle pad and C-stick, -1..1 (y down), and touch as a fraction of the whole
    // composed frame (the core works out which screen was touched).
    float circleX = 0, circleY = 0, cstickX = 0, cstickY = 0;
    bool pointerDown = false;
    float pointerX = 0, pointerY = 0;
};

// Key bits beyond the DS's twelve.
constexpr uint32_t kKeyZL = 1u << 12;
constexpr uint32_t kKeyZR = 1u << 13;
constexpr uint32_t kKeyHome = 1u << 14;

// Where the current frame's pixels are.
struct FrameInfo
{
    bool hardware = false;         // true: `texture` (GL) or `vkTexture` is an array, one layer per screen
    void* vkTexture = nullptr;     // ds13r::vk::Texture* from the Vulkan renderer
    uint64_t vkImage = 0;          // or a VkImage from another Vulkan renderer (3DS), 1 layer
    int vkFormat = 0;              // its VkFormat
    const void* screens[2] = {};   // software: 32-bit pixels per screen
    unsigned texture = 0;
    int width = 0, height = 0;     // one screen, in pixels
    int screenCount = 1;
    bool bgra = false;             // software pixel order: true = BGRA (melonDS), false = RGBA
};

class EmuCore : public AudioSource
{
public:
    ~EmuCore() override = default;

    virtual const char* SystemName() const = 0;
    virtual double NativeFps() const = 0;

    // Called on the emulation thread whenever settings may have changed.
    virtual void ApplySettings() = 0;

    virtual void RunFrame(const CoreInput& input) = 0;
    virtual bool GetFrame(FrameInfo& out) = 0;

    virtual void Reset() = 0;
    virtual void Shutdown() = 0;

    // Battery saves.
    virtual void CheckFlush() = 0;
    virtual void FlushSaves() = 0;

    // Save states (complete, self-contained snapshots).
    virtual bool SaveState(std::vector<uint8_t>& out) = 0;
    virtual bool LoadState(const uint8_t* data, size_t len) = 0;
    // Fast snapshot for rewind into a caller-owned buffer; returns the length, 0 on failure.
    virtual size_t CaptureState(uint8_t* buffer, size_t capacity) = 0;
    virtual size_t MaxStateSize() const = 0;

    // Enabled cheats, one entry per cheat, as the code text the player typed or imported.
    virtual void SetCheats(const std::vector<std::string>& codes) = 0;

    // Audio rate correction: >1 produces slightly fewer samples per emulated second.
    virtual void SetAudioSkew(double skew) = 0;
    virtual int AudioFill() = 0; // buffered output frames

    // Upscaling (hardware renderers only).
    virtual int RenderScale() const { return 1; }
    virtual bool UsesHardwareRenderer() const { return false; }
    // True if frames come from Vulkan, so the Vulkan presenter must show them.
    virtual bool WantsVulkanOutput() const { return false; }

    // Accessory inputs.
    virtual void SetSolarLevel(int delta) {}
    virtual void SyncClock() {}
};

}
