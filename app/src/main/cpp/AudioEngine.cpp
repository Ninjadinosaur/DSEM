#include "AudioEngine.h"
#include "LogBuffer.h"

#include <algorithm>
#include <cstring>

namespace ds13r
{

namespace
{
// melonDS expects microphone samples at the DS's own rate.
constexpr double kMicOutputRate = 47743.4659091;
constexpr size_t kMicRingSize = 16384;
}

class AudioEngine::MicCallback : public oboe::AudioStreamDataCallback
{
public:
    explicit MicCallback(AudioEngine& e) : engine(e) {}

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) override
    {
        const int16_t* in = static_cast<const int16_t*>(audioData);
        double step = (double)stream->getSampleRate() / kMicOutputRate;

        std::lock_guard<std::mutex> l(engine.micLock);
        // Linear resampler from the device rate (48 kHz) to 47.74 kHz.
        double pos = engine.micResamplePos;
        while (pos < numFrames - 1)
        {
            int i = (int)pos;
            double frac = pos - i;
            int16_t s = (int16_t)(in[i] + (in[i + 1] - in[i]) * frac);

            if (engine.micLevel < engine.micRing.size())
            {
                engine.micRing[engine.micWrite] = s;
                engine.micWrite = (engine.micWrite + 1) % engine.micRing.size();
                engine.micLevel++;
            }
            pos += step;
        }
        engine.micResamplePos = pos - (numFrames - 1);
        return oboe::DataCallbackResult::Continue;
    }

private:
    AudioEngine& engine;
};

AudioEngine::AudioEngine()
{
    micRing.resize(kMicRingSize);
}

AudioEngine::~AudioEngine()
{
    StopMic();
    StopOutput();
}

bool AudioEngine::OpenOutput()
{
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Exclusive)
        ->setFormat(oboe::AudioFormat::I16)
        ->setChannelCount(oboe::ChannelCount::Stereo)
        ->setSampleRate(kSampleRate)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setUsage(oboe::Usage::Game)
        ->setContentType(oboe::ContentType::Music)
        ->setDataCallback(this)
        ->setErrorCallback(this);

    oboe::Result r = builder.openStream(outStream);
    if (r != oboe::Result::OK)
    {
        LOGE("Audio: failed to open output stream: %s", oboe::convertToText(r));
        outStream.reset();
        return false;
    }

    ApplyBufferSize();

    r = outStream->requestStart();
    if (r != oboe::Result::OK)
    {
        LOGE("Audio: failed to start output stream: %s", oboe::convertToText(r));
        outStream->close();
        outStream.reset();
        return false;
    }

    LOGI("Audio: output %d Hz, burst %d frames, buffer %d frames, %s, %s",
         outStream->getSampleRate(), outStream->getFramesPerBurst(), outStream->getBufferSizeInFrames(),
         outStream->getSharingMode() == oboe::SharingMode::Exclusive ? "exclusive" : "shared",
         outStream->getPerformanceMode() == oboe::PerformanceMode::LowLatency ? "low latency" : "normal latency");
    return true;
}

void AudioEngine::ApplyBufferSize()
{
    if (!outStream) return;
    // Two bursts is the lowest reliable size; Bluetooth users can add more on top.
    int frames = outStream->getFramesPerBurst() * 2 + extraLatencyMs.load() * outStream->getSampleRate() / 1000;
    frames = std::min(frames, outStream->getBufferCapacityInFrames());
    outStream->setBufferSizeInFrames(frames);
}

bool AudioEngine::StartOutput()
{
    std::lock_guard<std::mutex> l(outLock);
    if (outStream) return true;
    return OpenOutput();
}

void AudioEngine::StopOutput()
{
    std::lock_guard<std::mutex> l(outLock);
    if (outStream)
    {
        outStream->stop();
        outStream->close();
        outStream.reset();
    }
}

void AudioEngine::SetExtraLatencyMs(int ms)
{
    extraLatencyMs.store(std::max(0, ms));
    std::lock_guard<std::mutex> l(outLock);
    ApplyBufferSize();
}

void AudioEngine::SetSource(AudioSource* src)
{
    std::lock_guard<std::mutex> l(sourceLock);
    source = src;
}

oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames)
{
    int16_t* out = static_cast<int16_t*>(audioData);
    int got = 0;

    {
        std::lock_guard<std::mutex> l(sourceLock);
        if (source) got = source->ReadAudio(out, numFrames);
    }

    if (got <= 0)
    {
        memset(out, 0, (size_t)numFrames * 2 * sizeof(int16_t));
        lastFrame = 0;
        return oboe::DataCallbackResult::Continue;
    }

    // Underrun: hold the last sample instead of dropping to zero, which would click.
    uint32_t* frames = reinterpret_cast<uint32_t*>(out);
    lastFrame = frames[got - 1];
    for (int i = got; i < numFrames; i++) frames[i] = lastFrame;

    if (muted.load())
    {
        memset(out, 0, (size_t)numFrames * 2 * sizeof(int16_t));
    }
    else
    {
        float v = volume.load();
        if (v < 0.999f)
        {
            int vol = (int)(v * 256.0f);
            for (int i = 0; i < numFrames * 2; i++) out[i] = (int16_t)((out[i] * vol) >> 8);
        }
    }
    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error)
{
    // Typically a device change, e.g. Bluetooth headphones connecting. Reopen on the new route.
    LOGW("Audio: stream closed (%s), reopening", oboe::convertToText(error));
    std::lock_guard<std::mutex> l(outLock);
    if (stream == outStream.get())
    {
        outStream.reset();
        OpenOutput();
    }
}

void AudioEngine::SetMicAllowed(bool allowed)
{
    bool start;
    {
        std::lock_guard<std::mutex> l(micLock);
        micAllowed = allowed;
        start = allowed && micWanted && !micStream;
    }
    if (start) StartMic();
    if (!allowed) StopMic();
}

void AudioEngine::StartMic()
{
    std::lock_guard<std::mutex> l(micLock);
    micWanted = true;
    if (micStream || !micAllowed) return;

    micCallback = std::make_unique<MicCallback>(*this);
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::I16)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(kSampleRate)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setInputPreset(oboe::InputPreset::Unprocessed)
        ->setDataCallback(micCallback.get());

    oboe::Result r = builder.openStream(micStream);
    if (r != oboe::Result::OK || micStream->requestStart() != oboe::Result::OK)
    {
        LOGE("Audio: failed to open microphone: %s", oboe::convertToText(r));
        if (micStream) micStream->close();
        micStream.reset();
        return;
    }
    micRead = micWrite = micLevel = 0;
    micResamplePos = 0.0;
    LOGI("Audio: microphone opened");
}

void AudioEngine::StopMic()
{
    std::shared_ptr<oboe::AudioStream> s;
    {
        std::lock_guard<std::mutex> l(micLock);
        micWanted = false;
        s = std::move(micStream);
        micStream.reset();
    }
    if (s)
    {
        s->stop();
        s->close();
        LOGI("Audio: microphone closed");
    }
}

int AudioEngine::ReadMic(int16_t* dst, int maxSamples)
{
    std::lock_guard<std::mutex> l(micLock);
    int n = (int)std::min<size_t>(maxSamples, micLevel);
    for (int i = 0; i < n; i++)
    {
        dst[i] = micRing[micRead];
        micRead = (micRead + 1) % micRing.size();
    }
    micLevel -= n;
    return n;
}

}
