#pragma once

#include "EmuCore.h"
#include "SaveFile.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct mCore;

namespace ds13r
{

// Game Boy Advance, emulated by mGBA.
class GbaCore : public EmuCore
{
public:
    explicit GbaCore(CoreHost& host);
    ~GbaCore() override;

    // Returns an empty string on success, otherwise a message for the player.
    std::string LoadGame(std::unique_ptr<uint8_t[]> rom, uint32_t len, const std::string& gameKey);

    // True if the data looks like a GBA ROM (fixed header byte and Nintendo logo checksum area).
    static bool LooksLikeGba(const uint8_t* data, size_t len);

    const char* SystemName() const override { return "GBA"; }
    double NativeFps() const override { return 59.7275; }
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

    CoreHost& Host() { return host; }

    struct Peripherals; // mGBA callback structs (rumble, tilt, light sensor)

private:
    void DrainAudio();
    void SnapshotSave(bool force);

    CoreHost& host;
    mCore* core = nullptr;
    std::unique_ptr<Peripherals> peripherals;
    std::vector<uint32_t> video;           // 240x160, RGBX8888
    std::unique_ptr<uint8_t[]> romData;    // must outlive the core
    uint32_t romLen = 0;
    uint8_t* saveMemory = nullptr;         // 128 KB, the largest GBA save type
    std::vector<uint8_t> lastSave;
    std::unique_ptr<SaveFile> saveFile;
    int framesSinceSaveCheck = 0;
    double skew = 1.0;

    // Audio ring: filled after each frame, drained by the audio thread.
    std::mutex audioLock;
    std::vector<int16_t> audioRing;
    size_t audioRead = 0, audioWrite = 0, audioLevel = 0;
    std::vector<int16_t> audioScratch;
};

}
