#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <oboe/Oboe.h>

#include "EmuCore.h"

// Low-latency audio through AAudio via Oboe (features.md §4).
// Output: pulls resampled 48 kHz stereo straight from the running core (melonDS or mGBA)
// on Oboe's real-time callback thread. Input: the phone microphone, opened only while a game
// is actually listening (as in melonDS 1.1).
namespace ds13r
{

class AudioEngine : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback
{
public:
    static constexpr int kSampleRate = 48000;

    AudioEngine();
    ~AudioEngine() override;

    bool StartOutput();
    void StopOutput();

    // Where samples come from; nullptr disconnects. Returns only once no read is in progress.
    void SetSource(AudioSource* source);

    void SetVolume(float v) { volume.store(v); }
    void SetMuted(bool m) { muted.store(m); }
    // Extra output buffering for Bluetooth headphones, in milliseconds (features.md §4).
    void SetExtraLatencyMs(int ms);

    // Microphone. Start/Stop are called when the emulated game opens/closes the mic.
    void SetMicAllowed(bool allowed);
    void StartMic();
    void StopMic();
    bool MicActive() const { return micStream != nullptr; }
    // Reads up to maxSamples mono samples (resampled to the rate melonDS expects).
    int ReadMic(int16_t* dst, int maxSamples);

    // oboe callbacks
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    class MicCallback;

    bool OpenOutput();
    void ApplyBufferSize();

    std::shared_ptr<oboe::AudioStream> outStream;
    std::mutex outLock;

    std::mutex sourceLock;
    AudioSource* source = nullptr;

    std::atomic<float> volume {1.0f};
    std::atomic<bool> muted {false};
    std::atomic<int> extraLatencyMs {0};
    uint32_t lastFrame = 0;

    // Microphone
    std::shared_ptr<oboe::AudioStream> micStream;
    std::unique_ptr<MicCallback> micCallback;
    std::mutex micLock;
    std::vector<int16_t> micRing;
    size_t micRead = 0, micWrite = 0, micLevel = 0;
    double micResamplePos = 0.0;
    bool micAllowed = false;
    bool micWanted = false;
};

}
