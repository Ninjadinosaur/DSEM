// flags.h must come first: it carries the options mGBA was built with, which change the
// layout of its structs.
#include <mgba/flags.h>

#include "GbaCore.h"
#include "ConfigStore.h"
#include "IoWorker.h"
#include "LogBuffer.h"

#include <mgba/core/blip_buf.h>
#include <mgba/core/cheats.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/gba/core.h>
#include <mgba/gba/interface.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>

namespace ds13r
{

namespace
{
constexpr int kWidth = 240;
constexpr int kHeight = 160;
constexpr size_t kSaveMemorySize = 0x20000; // Flash 1M, the largest GBA save
constexpr int kSampleRate = 48000;
constexpr size_t kAudioRingFrames = 8192;

// Route mGBA's log output into ours (warnings and errors only; mGBA is chatty at debug level).
struct mLogger gLogger;
void LogCallback(struct mLogger*, int category, enum mLogLevel level, const char* format, va_list args)
{
    if (!(level & (mLOG_WARN | mLOG_ERROR | mLOG_FATAL))) return;
    char buf[512];
    vsnprintf(buf, sizeof(buf), format, args);
    if (level & (mLOG_ERROR | mLOG_FATAL))
        LOGE("GBA [%s] %s", mLogCategoryName(category), buf);
    else
        LOGW("GBA [%s] %s", mLogCategoryName(category), buf);
}
}

// Phone hardware standing in for cartridge accessories (features.md §10).
struct GbaCore::Peripherals
{
    struct Rumble
    {
        mRumble base;
        GbaCore* self;
        bool on = false;
    } rumble;
    struct Rotation
    {
        mRotationSource base;
        GbaCore* self;
        int32_t tiltX = 0, tiltY = 0, gyroZ = 0;
    } rotation;
    struct Light
    {
        GBALuminanceSource base;
        GbaCore* self;
        uint8_t level = 0;
    } light;
};

static void RumbleSet(mRumble* r, int enable)
{
    auto* p = reinterpret_cast<GbaCore::Peripherals::Rumble*>(r);
    bool on = enable != 0;
    if (on == p->on) return;
    p->on = on;
    // Rumble Pak -> phone vibration motor: run until the game switches it off.
    p->self->Host().OnRumble(on ? 10000 : 0);
}

static void RotationSample(mRotationSource* s)
{
    auto* p = reinterpret_cast<GbaCore::Peripherals::Rotation*>(s);
    CoreHost& host = p->self->Host();
    p->tiltX = (int32_t)(host.MotionQuery(0) * -2e8f);
    p->tiltY = (int32_t)(host.MotionQuery(1) * 2e8f);
    p->gyroZ = (int32_t)(host.MotionQuery(5) * -5.5e8f);
}
static int32_t RotationTiltX(mRotationSource* s) { return reinterpret_cast<GbaCore::Peripherals::Rotation*>(s)->tiltX; }
static int32_t RotationTiltY(mRotationSource* s) { return reinterpret_cast<GbaCore::Peripherals::Rotation*>(s)->tiltY; }
static int32_t RotationGyroZ(mRotationSource* s) { return reinterpret_cast<GbaCore::Peripherals::Rotation*>(s)->gyroZ; }

static void LightSample(GBALuminanceSource* l)
{
    auto* p = reinterpret_cast<GbaCore::Peripherals::Light*>(l);
    p->level = (uint8_t)std::clamp(p->self->Host().LightLevel() * 255.0f, 0.0f, 255.0f);
}
static uint8_t LightRead(GBALuminanceSource* l)
{
    // Boktai's sensor reports darkness: 0xFF = no light.
    return 0xFF - reinterpret_cast<GbaCore::Peripherals::Light*>(l)->level;
}

bool GbaCore::LooksLikeGba(const uint8_t* data, size_t len)
{
    // 0xB2 is a fixed 0x96 in every GBA cartridge header; the entry point is an ARM branch.
    return len >= 0xC0 && data[0xB2] == 0x96 && data[3] == 0xEA;
}

GbaCore::GbaCore(CoreHost& h) : host(h), peripherals(std::make_unique<Peripherals>())
{
    static bool loggerSet = false;
    if (!loggerSet)
    {
        gLogger.log = LogCallback;
        gLogger.filter = nullptr;
        mLogSetDefaultLogger(&gLogger);
        loggerSet = true;
    }
    audioRing.resize(kAudioRingFrames * 2);
}

GbaCore::~GbaCore()
{
    Shutdown();
}

std::string GbaCore::LoadGame(std::unique_ptr<uint8_t[]> rom, uint32_t len, const std::string& gameKey)
{
    Shutdown();

    romData = std::move(rom);
    romLen = len;
    VFile* romFile = VFileFromConstMemory(romData.get(), romLen);
    if (!romFile) return "The game file could not be read.";

    core = GBACoreCreate();
    if (!core || !core->isROM(romFile))
    {
        romFile->close(romFile);
        if (core) core->deinit(core);
        core = nullptr;
        return "This file is not a valid GBA game.";
    }

    mCoreInitConfig(core, nullptr);
    core->init(core);

    video.assign((size_t)kWidth * kHeight, 0);
    core->setVideoBuffer(core, reinterpret_cast<color_t*>(video.data()), kWidth);

    // About one frame of audio inside mGBA; we drain it into our own ring after every frame.
    size_t perFrame = (size_t)((double)kSampleRate * core->frameCycles(core) / core->frequency(core));
    core->setAudioBufferSize(core, std::min<size_t>(perFrame * 2, 0x4000));
    SetAudioSkew(1.0);

    auto& per = *peripherals;
    per.rumble.self = this;
    per.rumble.base.setRumble = RumbleSet;
    per.rotation.self = this;
    per.rotation.base.sample = RotationSample;
    per.rotation.base.readTiltX = RotationTiltX;
    per.rotation.base.readTiltY = RotationTiltY;
    per.rotation.base.readGyroZ = RotationGyroZ;
    per.light.self = this;
    per.light.base.sample = LightSample;
    per.light.base.readLuminance = LightRead;
    core->setPeripheral(core, mPERIPH_RUMBLE, &per.rumble.base);
    core->setPeripheral(core, mPERIPH_ROTATION, &per.rotation.base);
    core->setPeripheral(core, mPERIPH_GBA_LUMINANCE, &per.light.base);

    if (!core->loadROM(core, romFile))
    {
        Shutdown();
        return "This file is not a valid GBA game.";
    }

    // Battery save: mGBA writes into this buffer; we persist it with the crash-safe writer.
    std::string savePath = host.FilesDir() + "/saves/" + gameKey + ".sav";
    SaveFile::BackupExisting(savePath, host.FilesDir() + "/backups/" + gameKey, (int)host.Config().GetInt("saves.backupCount", 10));
    saveMemory = static_cast<uint8_t*>(anonymousMemoryMap(kSaveMemorySize));
    memset(saveMemory, 0xFF, kSaveMemorySize);
    std::vector<unsigned char> existing;
    if (ReadWholeFile(savePath, existing) && !existing.empty())
        memcpy(saveMemory, existing.data(), std::min(existing.size(), kSaveMemorySize));
    core->loadSave(core, VFileFromMemory(saveMemory, kSaveMemorySize));
    saveFile = std::make_unique<SaveFile>(savePath);
    lastSave.assign(existing.begin(), existing.end());

    // Optional real BIOS; mGBA's built-in replacement is used otherwise.
    std::string bios = host.Config().GetString("bios.gba", "");
    if (host.Config().GetBool("emu.externalBios", false) && !bios.empty())
    {
        if (VFile* vf = VFileOpen(bios.c_str(), O_RDONLY))
        {
            if (core->loadBIOS(core, vf, 0))
            {
                core->opts.useBios = true;
                core->opts.skipBios = true; // straight into the game, no boot logo
            }
            else
            {
                vf->close(vf);
            }
        }
    }

    ApplySettings();
    core->reset(core);

    char title[17] = {};
    char code[9] = {};
    core->getGameTitle(core, title);
    core->getGameCode(core, code);
    LOGI("GBA: loaded %s (%s), %u bytes, save %zu bytes", title, code, romLen, existing.size());
    return {};
}

void GbaCore::ApplySettings()
{
    // Nothing renderer-related: mGBA draws in software at native 240x160 and the presenter scales.
}

void GbaCore::RunFrame(const CoreInput& input)
{
    if (!core) return;
    // DS key bits 0-9 are exactly the GBA's (A, B, Select, Start, Right, Left, Up, Down, R, L).
    core->setKeys(core, input.keys & 0x3FF);
    core->runFrame(core);
    DrainAudio();

    if (++framesSinceSaveCheck >= 60)
    {
        framesSinceSaveCheck = 0;
        SnapshotSave(false);
    }
}

void GbaCore::DrainAudio()
{
    blip_t* left = core->getAudioChannel(core, 0);
    blip_t* right = core->getAudioChannel(core, 1);
    int avail = blip_samples_avail(left);
    if (avail <= 0) return;
    audioScratch.resize((size_t)avail * 2);
    blip_read_samples(left, audioScratch.data(), avail, true);
    blip_read_samples(right, audioScratch.data() + 1, avail, true);

    std::lock_guard<std::mutex> l(audioLock);
    for (int i = 0; i < avail; i++)
    {
        if (audioLevel >= kAudioRingFrames)
        {
            // Full: drop the oldest frame rather than the newest.
            audioRead = (audioRead + 1) % kAudioRingFrames;
            audioLevel--;
        }
        audioRing[audioWrite * 2] = audioScratch[(size_t)i * 2];
        audioRing[audioWrite * 2 + 1] = audioScratch[(size_t)i * 2 + 1];
        audioWrite = (audioWrite + 1) % kAudioRingFrames;
        audioLevel++;
    }
}

int GbaCore::ReadAudio(int16_t* out, int frames)
{
    std::lock_guard<std::mutex> l(audioLock);
    int n = (int)std::min<size_t>((size_t)frames, audioLevel);
    for (int i = 0; i < n; i++)
    {
        out[i * 2] = audioRing[audioRead * 2];
        out[i * 2 + 1] = audioRing[audioRead * 2 + 1];
        audioRead = (audioRead + 1) % kAudioRingFrames;
    }
    audioLevel -= (size_t)n;
    return n;
}

int GbaCore::AudioFill()
{
    std::lock_guard<std::mutex> l(audioLock);
    return (int)audioLevel;
}

void GbaCore::SetAudioSkew(double s)
{
    skew = s;
    if (!core) return;
    double clock = core->frequency(core) * skew;
    blip_set_rates(core->getAudioChannel(core, 0), clock, kSampleRate);
    blip_set_rates(core->getAudioChannel(core, 1), clock, kSampleRate);
}

bool GbaCore::GetFrame(FrameInfo& out)
{
    if (!core) return false;
    out.hardware = false;
    out.screenCount = 1;
    out.screens[0] = video.data();
    out.screens[1] = nullptr;
    out.width = kWidth;
    out.height = kHeight;
    out.bgra = false; // mGBA's 32-bit colour is R,G,B,X in memory
    return true;
}

void GbaCore::Reset()
{
    if (core) core->reset(core);
}

void GbaCore::Shutdown()
{
    if (core)
    {
        SnapshotSave(true);
        if (saveFile) saveFile->FlushNow();
        mCoreConfigDeinit(&core->config);
        core->deinit(core);
        core = nullptr;
    }
    saveFile.reset();
    if (saveMemory)
    {
        mappedMemoryFree(saveMemory, kSaveMemorySize);
        saveMemory = nullptr;
    }
    romData.reset();
    romLen = 0;
    std::lock_guard<std::mutex> l(audioLock);
    audioRead = audioWrite = audioLevel = 0;
}

// Checks whether the game changed its save and, if so, hands it to the save writer.
void GbaCore::SnapshotSave(bool force)
{
    if (!core || !saveFile) return;
    void* sram = nullptr;
    size_t size = core->savedataClone(core, &sram);
    if (!sram || size == 0) return;
    bool changed = size != lastSave.size() || memcmp(sram, lastSave.data(), size) != 0;
    if (changed)
    {
        lastSave.assign(static_cast<uint8_t*>(sram), static_cast<uint8_t*>(sram) + size);
        saveFile->OnWrite(lastSave.data(), (uint32_t)size, 0, (uint32_t)size);
    }
    free(sram);
    if (force) saveFile->FlushNow();
}

void GbaCore::CheckFlush()
{
    if (saveFile) saveFile->CheckFlush();
}

void GbaCore::FlushSaves()
{
    SnapshotSave(true);
}

bool GbaCore::SaveState(std::vector<uint8_t>& out)
{
    if (!core) return false;
    VFile* vf = VFileMemChunk(nullptr, 0);
    bool ok = mCoreSaveStateNamed(core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
    if (ok)
    {
        out.resize((size_t)vf->size(vf));
        vf->seek(vf, 0, SEEK_SET);
        vf->read(vf, out.data(), out.size());
    }
    vf->close(vf);
    return ok;
}

bool GbaCore::LoadState(const uint8_t* data, size_t len)
{
    if (!core) return false;
    // Rewind snapshots are raw core states; save-state files use mGBA's full format.
    if (len == core->stateSize(core)) return core->loadState(core, data);
    VFile* vf = VFileFromConstMemory(data, len);
    bool ok = mCoreLoadStateNamed(core, vf, SAVESTATE_RTC);
    vf->close(vf);
    return ok;
}

size_t GbaCore::CaptureState(uint8_t* buffer, size_t capacity)
{
    if (!core) return 0;
    size_t size = core->stateSize(core);
    if (size > capacity || !core->saveState(core, buffer)) return 0;
    return size;
}

size_t GbaCore::MaxStateSize() const
{
    return core ? core->stateSize(core) : 0x80000;
}

void GbaCore::SetCheats(const std::vector<std::string>& codes)
{
    if (!core) return;
    mCheatDevice* device = core->cheatDevice(core);
    if (!device) return;
    mCheatDeviceClear(device);
    size_t added = 0;
    for (const std::string& text : codes)
    {
        mCheatSet* set = device->createSet(device, nullptr);
        bool ok = true;
        // One code per line; mGBA recognises GameShark, Action Replay and CodeBreaker formats.
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find('\n', start);
            std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
            if (!line.empty() && !mCheatAddLine(set, line.c_str(), 0)) ok = false;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (!ok)
        {
            LOGW("GBA cheats: skipped a code mGBA could not read");
            mCheatSetDeinit(set);
            continue;
        }
        mCheatAddSet(device, set);
        added++;
    }
    LOGI("GBA cheats: %zu active", added);
}

}
