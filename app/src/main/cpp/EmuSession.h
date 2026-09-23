#pragma once

#include "AudioEngine.h"
#include "ConfigStore.h"
#include "EmuCore.h"
#include "Presenter.h"
#include "RewindBuffer.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <jni.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ANativeWindow;

namespace ds13r
{

class DsCore;
class GLPresenter;
class PerfManager;

// Callbacks from native code up to the Kotlin layer.
struct HostCallbacks
{
    JavaVM* vm = nullptr;
    jobject bridge = nullptr;          // global ref to the Kotlin NativeBridge callback object
    jmethodID onMicRequest = nullptr;  // (Z)V  game opened/closed the microphone
    jmethodID onRumble = nullptr;      // (I)V  milliseconds, 0 = stop
    jmethodID onRtcOffset = nullptr;   // (J)V  game changed the clock; persist this offset
    jmethodID onEmuStopped = nullptr;  // (I)V  melonDS StopReason
    jmethodID onThermal = nullptr;     // (II)V thermal status, new scale factor
};

struct EmuStats
{
    float fps = 0;             // frames emulated per second
    float speed = 0;           // percent of real console speed
    float frameTimeMs = 0;     // average CPU time per emulated frame
    float presentFps = 0;      // frames shown per second
    float thermalHeadroom = -1;
    int thermalStatus = 0;
    int renderScale = 1;
    float rewindSeconds = 0;
    float rewindMemoryMb = 0;
};

enum class SpeedMode { Normal = 0, FastForward = 1, SlowMotion = 2 };

// Owns the emulation thread and everything shared by the DS and GBA cores:
// display, audio, input, frame pacing, save states, rewind, speed control and thermals.
class EmuSession : public CoreHost
{
public:
    static EmuSession& Get();

    void Init(JavaVM* vm, jobject activity, const std::string& filesDir);
    void SetCallbacks(const HostCallbacks& cb);

    // Loads a DS, GBA or 3DS game (detected from the file) from an open file descriptor.
    // `gameKey` names the game's saves and states. Returns "" or a message for the player.
    std::string LoadGame(int fd, const std::string& fileName, const std::string& gameKey);
    // Boots the DS firmware menu without a cartridge (needs real BIOS/firmware dumps).
    std::string BootFirmware();
    // Inserts a GBA game into the DS's GBA slot (only while a DS game runs).
    std::string LoadGbaSlotRom(int fd, const std::string& fileName);
    // "DS", "GBA", "3DS" or "" when nothing is loaded.
    std::string SystemName();

    void Start();
    void SetPaused(bool paused);
    bool IsPaused() const { return paused.load(); }
    void Reset();
    void Stop();
    void FlushSaves();

    // Display
    void SetSurface(ANativeWindow* window);
    void SetLayout(const PresentLayout& layout);
    void SetPresentSettings(const PresentSettings& settings);

    // Input (any thread)
    void SetKeys(uint32_t pressedMask);
    void SetTouch(bool down, int x, int y);
    // 3DS: stick 0 = circle pad, 1 = C-stick, -1..1 (y down); touch as a fraction of the frame.
    void SetAnalog(int stick, float x, float y);
    void SetPointer(bool down, float x, float y);
    // 3DS: size of the composed frame (both screens), or false when no 3DS game runs.
    bool ThreeDsFrameSize(int& width, int& height);
    void SetLidClosed(bool closed);
    void SetBlow(bool active) { blowActive.store(active); }
    void SetMicAllowed(bool allowed) { audio.SetMicAllowed(allowed); }
    void SetGuitarKeys(uint32_t mask) { guitarKeys.store(mask); }
    void SetMotion(const float accel[3], const float gyro[3]);
    void SetLight(float level) { light.store(level); }
    void SetSolarLevel(int delta);

    // Speed
    void SetSpeedMode(SpeedMode mode);
    void FrameAdvance();

    // Save states (run on the emulation thread; these block until done)
    bool SaveState(const std::string& path, const std::string& thumbPath);
    bool LoadState(const std::string& path);
    bool UndoLoadState();
    bool UndoSaveState(const std::string& path);

    void SetRewinding(bool active);

    // Screenshot: all screens stacked, RGBA; returns the size of one screen.
    bool Screenshot(std::vector<uint32_t>& pixels, int& width, int& height, int& screens);

    // Enabled cheats, one code text per cheat.
    void SetCheats(const std::vector<std::string>& codes);

    void ApplyLiveSettings();
    EmuStats GetStats();

    // ---- CoreHost ----
    ConfigStore& Config() override { return config; }
    const std::string& FilesDir() const override { return filesDir; }
    GLPresenter* Presenter() override { return presenter.get(); }
    void OnMicStart() override;
    void OnMicStop() override;
    int ReadMic(int16_t* data, int maxlen) override;
    void OnRumble(uint32_t ms) override;
    void OnRtcOffset(int64_t offsetSeconds) override;
    void OnCoreStopped(int reason) override;
    float MotionQuery(int type) override;
    bool GuitarKeyDown(int key) override;
    float LightLevel() override { return light.load(); }

private:
    EmuSession();

    void ThreadMain();
    void RunCommands();
    void Post(std::function<void()> fn, bool wait);
    void SwapCore(std::unique_ptr<EmuCore> next);
    void RunOneFrame();
    void PresentFrame();
    void UpdateAudioSync(double emulatedFps);
    void CaptureRewind();
    void DoRewindStep();
    void UpdateThermalScale();
    void ClearRewind();
    JNIEnv* ThreadEnv();

    ConfigStore config;
    std::string filesDir;
    JavaVM* vm = nullptr;
    jobject activity = nullptr;
    HostCallbacks callbacks;

    // Emulation thread and its command queue
    std::thread thread;
    std::mutex cmdLock;
    std::condition_variable cmdCv;
    std::deque<std::function<void()>> commands;
    std::atomic<bool> quit {false};
    std::atomic<bool> running {false};
    std::atomic<bool> paused {true};
    std::atomic<bool> frameStep {false};

    // The loaded system. Replaced only on the emulation thread, with the audio source detached.
    std::unique_ptr<EmuCore> core;
    DsCore* dsCore = nullptr; // same object as `core` while a DS game runs

    std::unique_ptr<GLPresenter> presenter;
    std::atomic<bool> needRedraw {false};

    AudioEngine audio;
    double audioSkew = 1.0;
    double fillAverage = -1.0;

    // Input
    std::atomic<uint32_t> keys {0};
    std::atomic<uint32_t> touchState {0};   // bit31 = down, x in bits 0-7, y in bits 8-15
    std::atomic<int> lidRequest {-1};
    std::atomic<uint32_t> circlePad {0}, cStick {0}; // x in the low 16 bits, y high, signed
    std::atomic<uint32_t> pointerState {0};           // bit31 = down, x bits 0-14, y bits 15-29
    std::atomic<bool> blowActive {false};
    std::atomic<uint32_t> guitarKeys {0};
    std::atomic<float> motion[6] {};
    std::atomic<float> light {0.5f};
    size_t blowPos = 0;

    // Speed
    std::atomic<int> speedMode {0};
    double frameAccumulator = 0.0;
    float currentRefresh = 60.0f;

    // Savestates
    std::vector<uint8_t> undoLoadBackup;

    // Rewind
    RewindBuffer rewind;
    std::mutex rewindLock;
    std::atomic<bool> rewinding {false};
    std::vector<uint8_t> rewindCapture;
    size_t rewindCaptureLen = 0;
    std::thread rewindWorker;
    std::condition_variable rewindCv;
    bool rewindPending = false;
    int rewindFrameCounter = 0;

    // Stats
    std::mutex statsLock;
    EmuStats stats;
    uint32_t statFrames = 0, statPresents = 0;
    double statWorkMs = 0;
    int64_t statStart = 0;

    std::unique_ptr<PerfManager> perf;
    int64_t lastThermalChange = 0;
    int64_t coolSince = 0;
};

}
