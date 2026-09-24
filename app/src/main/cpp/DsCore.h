#pragma once

#include "EmuCore.h"
#include "SaveFile.h"

#include <memory>
#include <string>

namespace melonDS { class NDS; }

namespace ds13r
{

// Nintendo DS / DSi, emulated by melonDS.
class DsCore : public EmuCore
{
public:
    explicit DsCore(CoreHost& host);
    ~DsCore() override;

    // Returns an empty string on success, otherwise a message for the player.
    std::string LoadGame(std::unique_ptr<uint8_t[]> rom, uint32_t len, const std::string& fileName, const std::string& gameKey);
    std::string BootFirmware();
    // Inserts a GBA cartridge into slot 2 (e.g. Pokemon migration).
    std::string InsertGbaCart(std::unique_ptr<uint8_t[]> rom, uint32_t len, const std::string& fileName);

    const char* SystemName() const override { return "DS"; }
    double NativeFps() const override { return 59.8261; }
    void ApplySettings() override;
    void RunFrame(const CoreInput& input) override;
    bool GetFrame(FrameInfo& out) override;
    void Reset() override;
    void Shutdown() override;
    void CheckFlush() override;
    void FlushSaves() override;
    bool SaveState(std::vector<uint8_t>& out) override;
    bool LoadState(const uint8_t* data, size_t len) override;
    size_t CaptureState(uint8_t* buffer, size_t capacity) override;
    size_t MaxStateSize() const override;
    void SetCheats(const std::vector<std::string>& codes) override;
    void SetAudioSkew(double skew) override;
    int AudioFill() override;
    int ReadAudio(int16_t* out, int frames) override;
    int RenderScale() const override;
    bool UsesHardwareRenderer() const override { return activeRenderer != 0; }
    bool WantsVulkanOutput() const override { return activeRenderer == 3; }
    void SetSolarLevel(int delta) override;
    void SyncClock() override;

    // melonDS Platform callbacks (userdata = this).
    CoreHost& Host() { return host; }
    void OnNdsSaveWrite(const uint8_t* data, uint32_t len, uint32_t off, uint32_t wlen);
    void OnGbaSaveWrite(const uint8_t* data, uint32_t len, uint32_t off, uint32_t wlen);
    void OnFirmwareWrite();
    void OnDateTimeWrite(int year, int month, int day, int hour, int minute, int second);

private:
    bool CreateConsole(std::string& error);
    void ApplyRtc();

    CoreHost& host;
    std::unique_ptr<melonDS::NDS> nds;
    int consoleType = 0;
    int activeRenderer = 0; // 0 software, 1 OpenGL ES, 2 OpenGL ES compute, 3 Vulkan
    std::string romName;
    std::string gameKey;

    std::unique_ptr<SaveFile> ndsSave;
    std::unique_ptr<SaveFile> gbaSave;
};

}
